#include "spectrum.h"
#include "engine.h"
#include "theme.h"
#include <QContextMenuEvent>
#include <QLocale>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <cmath>
#include <numbers>

static constexpr unsigned kN = 4096;           // ~10.8 Hz per bin at 44.1 kHz
static constexpr double kLoHz = 20;            // the axis ends at the track's Nyquist (rate / 2)
static constexpr double kFloorDb = -72;        // bottom of the view
static constexpr int kAnnounceMs = 1400;       // how long a newly chosen style shows its name
static const char *const kStyleNames[SpectrumView::StyleCount] = {"Curva", "Barras", "Bloques", "Espejo", "Línea", "Onda"};

SpectrumView::SpectrumView(Engine *engine, QWidget *parent) : QWidget(parent), engine_(engine)
{
    samples_.resize(kN);
    window_.resize(kN);
    fft_.resize(kN);
    twiddle_.resize(kN / 2);
    bitrev_.resize(kN);
    for (unsigned i = 0; i < kN; ++i)
        window_[i] = float(0.5 - 0.5 * std::cos(2 * std::numbers::pi * i / (kN - 1)));   // Hann
    for (unsigned i = 0; i < kN / 2; ++i) twiddle_[i] = std::polar(1.0f, float(-2 * std::numbers::pi * i / kN));
    const unsigned bits = unsigned(std::log2(kN));
    for (unsigned i = 0; i < kN; ++i) {
        unsigned r = 0;
        for (unsigned b = 0; b < bits; ++b) r |= ((i >> b) & 1u) << (bits - 1 - b);
        bitrev_[i] = r;
    }
    setMinimumHeight(60);
    setCursor(Qt::PointingHandCursor);
    setToolTip("Clic para cambiar de estilo · clic derecho para elegirlo");
    setAttribute(Qt::WA_OpaquePaintEvent);   // we paint the background ourselves (cached)
    timer_.setInterval(33);
    timer_.setTimerType(Qt::PreciseTimer);
    connect(&timer_, &QTimer::timeout, this, &SpectrumView::step);
    connect(engine_, &Engine::stateChanged, this, [this] {
        if (isVisible() && !timer_.isActive()) {
            clock_.start();
            timer_.start();
        }
    });
}

void SpectrumView::showEvent(QShowEvent *)
{
    engine_->setScopeEnabled(true);
    clock_.start();
    timer_.start();
}

void SpectrumView::hideEvent(QHideEvent *)
{
    engine_->setScopeEnabled(false);
    timer_.stop();
}

void SpectrumView::setStyle(int style, bool announce)
{
    style = std::clamp(style, 0, StyleCount - 1);
    if (style == style_ && !announce) return;
    style_ = style;
    if (announce) {
        announced_.start();
        QTimer::singleShot(kAnnounceMs + 50, this, qOverload<>(&QWidget::update));   // also goes away while paused
    }
    update();
}

void SpectrumView::mousePressEvent(QMouseEvent *e)
{
    if (e->button() == Qt::LeftButton) setStyle((style_ + 1) % StyleCount, true);
    else QWidget::mousePressEvent(e);
}

void SpectrumView::contextMenuEvent(QContextMenuEvent *e)
{
    QMenu menu(this);
    for (int i = 0; i < StyleCount; ++i) {
        QAction *a = menu.addAction(QString::fromUtf8(kStyleNames[i]));
        a->setCheckable(true);
        a->setChecked(i == style_);
        connect(a, &QAction::triggered, this, [this, i] { setStyle(i, true); });
    }
    menu.exec(e->globalPos());
}

void SpectrumView::analyse(int rate, int points)
{
    // in-place iterative radix-2 FFT of the windowed block
    for (unsigned i = 0; i < kN; ++i) fft_[bitrev_[i]] = {samples_[i] * window_[i], 0.0f};
    for (unsigned len = 2; len <= kN; len <<= 1) {
        const unsigned half = len / 2, stride = kN / len;
        for (unsigned i = 0; i < kN; i += len)
            for (unsigned j = 0; j < half; ++j) {
                const std::complex<float> t = fft_[i + j + half] * twiddle_[j * stride];
                fft_[i + j + half] = fft_[i + j] - t;
                fft_[i + j] += t;
            }
    }
    // amplitude of a full-scale sine = 0 dB (Hann window sums to N/2)
    const float norm = 2.0f / (kN / 2.0f);
    const double binHz = double(rate) / kN;
    const double hi = rate / 2.0;
    auto magAt = [&](double hz) {   // linear interpolation between bins, for the narrow low slices
        const double k = hz / binHz;
        const unsigned k0 = std::min<unsigned>(unsigned(k), kN / 2 - 2);
        const double fr = k - k0;
        return float((1 - fr) * std::abs(fft_[k0]) + fr * std::abs(fft_[k0 + 1]));
    };
    // one value per curve point: the energy of the slice of spectrum around it
    // (averaged, not the loudest bin, so the curve doesn't jitter)
    target_.assign(size_t(points), 0.0f);
    for (int i = 0; i < points; ++i) {
        const double u = double(i) / (points - 1), du = 0.5 / (points - 1);
        const double fc = kLoHz * std::pow(hi / kLoHz, u);
        const double f0 = kLoHz * std::pow(hi / kLoHz, u - du), f1 = kLoHz * std::pow(hi / kLoHz, u + du);
        const unsigned k0 = unsigned(std::ceil(f0 / binHz)), k1 = std::min(unsigned(f1 / binHz), kN / 2 - 1);
        float m;
        if (k1 >= k0) {
            double power = 0;
            for (unsigned k = k0; k <= k1; ++k) power += std::norm(fft_[k]);
            m = float(std::sqrt(power / (k1 - k0 + 1)));
        } else {
            m = magAt(fc);
        }
        // music falls ~3 dB per octave; tilt it back so the treble isn't flat on the floor
        const double db = 20 * std::log10(std::max(1e-9f, m * norm)) + 3 * std::log2(fc / 1000);
        target_[size_t(i)] = float(std::clamp((db - kFloorDb) / -kFloorDb, 0.0, 1.0));
    }
    // a light [1 2 1] blur along the axis: one continuous line, not a comb
    for (int pass = 0; pass < 2; ++pass) {
        float prev = target_[0];
        for (int i = 1; i + 1 < points; ++i) {
            const float cur = target_[size_t(i)];
            target_[size_t(i)] = (prev + 2 * cur + target_[size_t(i + 1)]) / 4;
            prev = cur;
        }
    }
}

void SpectrumView::step()
{
    const int points = std::clamp(width() / 6, 32, 260);
    const double dt = std::min(0.1, clock_.restart() / 1000.0);
    const bool playing = engine_->state() == Engine::Playing;
    if (playing) {
        rate_ = engine_->scopeSnapshot(samples_.data(), kN);
        analyse(rate_, points);
    } else {
        target_.assign(size_t(points), 0.0f);
        for (float &v : samples_) v = std::abs(v) < 1e-3f ? 0 : v * 0.75f;   // the waveform settles to a flat line
    }

    if (level_.size() != size_t(points)) {
        level_.assign(size_t(points), 0.0f);
        peak_.assign(size_t(points), 0.0f);
        peakHold_.assign(size_t(points), 0.0f);
    }
    // time-based easing so the curve breathes with the music instead of
    // following every frame: ~90 ms to rise, ~300 ms to fall
    const float up = float(1 - std::exp(-dt / 0.09)), down = float(1 - std::exp(-dt / 0.30));
    bool moving = false;
    for (int i = 0; i < points; ++i) {
        float &l = level_[size_t(i)], &p = peak_[size_t(i)], &h = peakHold_[size_t(i)];
        const float t = target_[size_t(i)];
        l += (t - l) * (t > l ? up : down);
        if (l < 0.002f) l = 0;
        if (l >= p) {
            p = l;
            h = 0.8f;
        } else if ((h -= float(dt)) < 0) p = std::max(l, p - float(dt * 0.25));
        moving |= l > 0 || p > 0.01f;
    }
    if (!playing && style_ == Wave)
        for (unsigned i = 0; i < kN && !moving; i += 8) moving = samples_[i] != 0;
    update();
    if (!playing && !moving) timer_.stop();   // the curve has settled; restarted on the next show
}

QRect SpectrumView::plotArea() const
{
    return QRect(0, 4, width(), height() - 16 - 4);   // 16 px for the frequency labels
}

static QString khz(double hz)
{
    if (hz < 1000) return QString::number(hz);
    QString s = QLocale(QLocale::Spanish).toString(hz / 1000, 'f', 2);   // "22,05"
    while (s.endsWith('0')) s.chop(1);
    if (s.endsWith(',')) s.chop(1);
    return s + 'k';
}

void SpectrumView::paintBackground()
{
    const Theme &t = Theme::get();
    const bool wave = style_ == Wave;   // time, not frequency: no axis
    const QString key = QString("%1x%2 %3 %4 %5 %6").arg(width()).arg(height()).arg(rate_).arg(t.bg.name(), t.accent.name()).arg(wave);
    if (key == bgKey_ && !bg_.isNull()) return;
    bgKey_ = key;
    bg_ = QPixmap(size() * devicePixelRatioF());
    bg_.setDevicePixelRatio(devicePixelRatioF());
    bg_.fill(t.bg);
    QPainter p(&bg_);
    QFont f = font();
    f.setPixelSize(10);
    p.setFont(f);
    const QRectF area = plotArea();
    const double hi = rate_ / 2.0;
    auto xOf = [&](double hz) { return area.left() + area.width() * std::log(hz / kLoHz) / std::log(hi / kLoHz); };
    QColor grid = t.muted;
    grid.setAlphaF(0.35);
    if (wave) {
        const double y = std::round(area.center().y()) + 0.5;
        p.setPen(QPen(grid, 1, Qt::DotLine));
        p.drawLine(QPointF(area.left(), y), QPointF(area.right(), y));
        return;
    }
    for (double hz : {50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0, 20000.0, 50000.0, 100000.0}) {
        const double x = std::round(xOf(hz)) + 0.5;
        if (x > area.right() - 44) break;   // leave room for the Nyquist label
        p.setPen(QPen(grid, 1, Qt::DotLine));
        p.drawLine(QPointF(x, area.top()), QPointF(x, area.bottom()));
        p.setPen(t.fgDim);
        p.drawText(QRectF(x - 20, area.bottom() + 2, 40, 16), Qt::AlignHCenter | Qt::AlignTop, khz(hz));
    }
    // the right edge is the highest frequency this sample rate can hold
    p.setPen(t.fgDim);
    p.drawText(QRectF(area.right() - 80, area.bottom() + 2, 78, 16), Qt::AlignRight | Qt::AlignTop, khz(hi));
    p.setPen(QPen(grid, 1));
    p.drawLine(area.bottomLeft(), area.bottomRight());
}

void SpectrumView::paintEvent(QPaintEvent *)
{
    const Theme &t = Theme::get();
    paintBackground();
    QPainter p(this);
    p.drawPixmap(0, 0, bg_);
    const QRectF area = plotArea();
    if (style_ == Wave) paintWave(p, area);
    else if (level_.size() < 2) {
        p.setPen(t.fgDim);
        p.drawText(area, Qt::AlignCenter, "El espectro aparecerá al reproducir");
    } else if (style_ == Curve || style_ == Line) paintCurve(p, area, style_ == Curve);
    else paintBars(p, area);

    if (announced_.isValid() && announced_.elapsed() < kAnnounceMs) {   // the style just chosen, top right
        p.setRenderHint(QPainter::Antialiasing);
        QFont f = font();
        f.setPixelSize(11);
        f.setBold(true);
        p.setFont(f);
        const QString name = QString::fromUtf8(kStyleNames[style_]);
        const QRectF box(area.right() - p.fontMetrics().horizontalAdvance(name) - 26, area.top() + 6,
                         p.fontMetrics().horizontalAdvance(name) + 18, 22);
        QColor back = t.bgLight;
        back.setAlphaF(0.92);
        p.setPen(QPen(t.accent, 1));
        p.setBrush(back);
        p.drawRoundedRect(box, 6, 6);
        p.setPen(t.fg);
        p.drawText(box, Qt::AlignCenter, name);
    } else announced_.invalidate();
}

void SpectrumView::paintCurve(QPainter &p, const QRectF &area, bool filled)
{
    const Theme &t = Theme::get();
    const int n = int(level_.size());
    // the points are already smoothed (blur + easing), so straight segments
    // read as a continuous curve; only the line itself is antialiased, the fill
    // under it isn't (its edge is covered by the line) — this runs 30 times a second
    auto curve = [&](const std::vector<float> &v) {
        QPolygonF poly(n);
        for (int i = 0; i < n; ++i)
            poly[i] = QPointF(area.left() + area.width() * i / (n - 1), area.bottom() + 1 - v[size_t(i)] * area.height());
        return poly;
    };
    const QPolygonF line = curve(level_);
    if (filled) {
        QPolygonF fill = line;
        fill << QPointF(area.right() + 1, area.bottom() + 1) << QPointF(area.left(), area.bottom() + 1);
        QLinearGradient g(area.bottomLeft(), area.topLeft());
        QColor lo = t.accent, top = t.accent.lighter(t.dark ? 130 : 105);
        lo.setAlphaF(0.12);
        top.setAlphaF(0.55);
        g.setColorAt(0, lo);
        g.setColorAt(1, top);
        p.setPen(Qt::NoPen);
        p.setBrush(g);
        p.drawPolygon(fill);
        p.setBrush(Qt::NoBrush);
    }
    p.setRenderHint(QPainter::Antialiasing);
    QColor peak = t.fgLight;
    peak.setAlphaF(0.35);
    p.setPen(QPen(peak, 1));
    p.drawPolyline(curve(peak_));
    // the curve, ~1.6 px thick: three 1 px strokes a fraction of a pixel apart
    // look the same as one wide pen, but take Qt's fast path for thin lines
    // instead of building and filling an outline (6x cheaper, every frame)
    p.setPen(QPen(t.accent.lighter(t.dark ? 120 : 100), 1));
    p.drawPolyline(line);
    p.drawPolyline(line.translated(0, -0.7));
    p.drawPolyline(line.translated(0.5, -0.35));
}

static QColor mix(const QColor &a, const QColor &b, double k)   // k of b over a
{
    return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * k, a.greenF() + (b.greenF() - a.greenF()) * k,
                            a.blueF() + (b.blueF() - a.blueF()) * k);
}

// Bars, Blocks (bars cut into LED-like segments) and Mirror (bars growing both
// ways from the middle). Whole-pixel rectangles, no antialiasing.
void SpectrumView::paintBars(QPainter &p, const QRectF &area)
{
    const Theme &t = Theme::get();
    const int n = int(level_.size());
    const int left = int(area.left()), top = int(area.top()), w = int(area.width()), h = int(area.height());
    const int bottom = top + h;
    const int bars = std::clamp(w / (style_ == Blocks ? 12 : 9), 8, n);
    const int gap = w / bars >= 5 ? 2 : 1;
    const int step = 4;   // Blocks: 3 px lit + 1 px dark
    const bool mirror = style_ == Mirror;
    const int cy = top + h / 2;

    const QColor dim = mix(t.bg, t.accent, 0.45), bright = t.accent.lighter(t.dark ? 125 : 105);
    QLinearGradient g(0, bottom, 0, top);
    if (mirror) {
        g.setColorAt(0, bright);
        g.setColorAt(0.5, dim);
        g.setColorAt(1, bright);
    } else {
        g.setColorAt(0, dim);
        g.setColorAt(1, bright);
    }
    const QBrush brush(g);
    QColor cap = t.fgLight;
    cap.setAlphaF(0.6);
    int tallest = 0;
    for (int j = 0; j < bars; ++j) {
        // a bar shows the loudest point of the stretch of curve it covers
        const int a = j * n / bars, b = std::max(a + 1, (j + 1) * n / bars);
        float lv = 0, pk = 0;
        for (int i = a; i < b; ++i) {
            lv = std::max(lv, level_[size_t(i)]);
            pk = std::max(pk, peak_[size_t(i)]);
        }
        const int x0 = left + j * w / bars, bw = std::max(1, left + (j + 1) * w / bars - gap - x0);
        if (mirror) {
            const int half = int(std::lround(lv * (h / 2)));
            if (half > 0) p.fillRect(x0, cy - half, bw, 2 * half, brush);
            else p.fillRect(x0, cy, bw, 1, dim);   // a thin spine while silent
            const int ph = int(std::lround(pk * (h / 2)));
            if (ph > half + 1) {
                p.fillRect(x0, cy - ph - 1, bw, 1, cap);
                p.fillRect(x0, cy + ph, bw, 1, cap);
            }
            continue;
        }
        int bh = int(std::lround(lv * h)), ph = int(std::lround(pk * h));
        if (style_ == Blocks) {
            bh -= bh % step;
            ph -= ph % step;
        }
        tallest = std::max(tallest, bh);
        if (bh > 0) p.fillRect(x0, bottom - bh, bw, bh, brush);
        if (ph > bh && ph > 2) p.fillRect(x0, bottom - ph, bw, style_ == Blocks ? step - 1 : 2, cap);
    }
    if (style_ == Blocks)   // cut the bars into segments: one dark row every `step` px, as far up as they reach
        for (int y = bottom - step; y >= bottom - tallest; y -= step) p.fillRect(left, y, w, 1, t.bg);
}

// The sound itself rather than its spectrum: ~35 ms of waveform, started at a
// rising zero crossing so that steady notes stand still instead of scrolling.
void SpectrumView::paintWave(QPainter &p, const QRectF &area)
{
    const Theme &t = Theme::get();
    const unsigned span = 1536, search = 1024;
    unsigned start = kN - span - search;
    for (unsigned i = start + 1; i < kN - span; ++i)
        if (samples_[i - 1] < 0 && samples_[i] >= 0) {
            start = i;
            break;
        }
    const int pts = std::clamp(int(area.width() / 2), 2, 512);
    const double cy = area.center().y(), half = area.height() / 2 - 1;
    QPolygonF line(pts);
    for (int i = 0; i < pts; ++i) {
        const double pos = double(i) * (span - 1) / (pts - 1);
        const unsigned k = start + unsigned(pos);
        const double fr = pos - unsigned(pos);
        const double v = samples_[k] * (1 - fr) + samples_[std::min(k + 1, kN - 1)] * fr;
        line[i] = QPointF(area.left() + area.width() * i / (pts - 1), cy - std::clamp(v, -1.0, 1.0) * half);
    }
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(t.accent.lighter(t.dark ? 120 : 100), 1));
    p.drawPolyline(line);
    p.drawPolyline(line.translated(0, -0.7));
    p.drawPolyline(line.translated(0.5, -0.35));
}
