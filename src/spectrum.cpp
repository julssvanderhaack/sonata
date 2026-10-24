#include "spectrum.h"
#include "engine.h"
#include "theme.h"
#include <QLocale>
#include <QPainter>
#include <cmath>
#include <numbers>

static constexpr unsigned kN = 4096;           // ~10.8 Hz per bin at 44.1 kHz
static constexpr double kLoHz = 20;            // the axis ends at the track's Nyquist (rate / 2)
static constexpr double kFloorDb = -72;        // bottom of the view

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
    } else target_.assign(size_t(points), 0.0f);

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
    const QString key = QString("%1x%2 %3 %4 %5").arg(width()).arg(height()).arg(rate_).arg(t.bg.name(), t.accent.name());
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
    const int n = int(level_.size());
    if (n < 2) {
        p.setPen(t.fgDim);
        p.drawText(area, Qt::AlignCenter, "El espectro aparecerá al reproducir");
        return;
    }
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
    p.setRenderHint(QPainter::Antialiasing);
    QColor peak = t.fgLight;
    peak.setAlphaF(0.35);
    p.setPen(QPen(peak, 1));
    p.drawPolyline(curve(peak_));
    p.setPen(QPen(t.accent.lighter(t.dark ? 120 : 100), 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.drawPolyline(line);
}
