#include "spectrum.h"
#include "engine.h"
#include "theme.h"
#include <QLocale>
#include <QPainter>
#include <cmath>
#include <numbers>

static constexpr unsigned kN = 4096;           // ~10.8 Hz per bin at 44.1 kHz
static constexpr double kLoHz = 30, kHiHz = 18000;
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

void SpectrumView::analyse(int rate, int bands)
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
    const double hi = std::min(kHiHz, rate / 2.0 * 0.95);
    auto magAt = [&](double hz) {   // linear interpolation between bins, for the narrow low bands
        const double k = hz / binHz;
        const unsigned k0 = std::min<unsigned>(unsigned(k), kN / 2 - 2);
        const double fr = k - k0;
        return float((1 - fr) * std::abs(fft_[k0]) + fr * std::abs(fft_[k0 + 1]));
    };
    target_.assign(size_t(bands), 0.0f);
    for (int b = 0; b < bands; ++b) {
        const double f0 = kLoHz * std::pow(hi / kLoHz, double(b) / bands);
        const double f1 = kLoHz * std::pow(hi / kLoHz, double(b + 1) / bands);
        const unsigned k0 = unsigned(std::ceil(f0 / binHz)), k1 = unsigned(f1 / binHz);
        float m = 0;
        if (k1 >= k0)
            for (unsigned k = k0; k <= k1 && k < kN / 2; ++k) m = std::max(m, std::abs(fft_[k]));
        else m = magAt(std::sqrt(f0 * f1));
        const double fc = std::sqrt(f0 * f1);
        // music falls ~3 dB per octave; tilt it back so the treble isn't flat on the floor
        const double db = 20 * std::log10(std::max(1e-9f, m * norm)) + 3 * std::log2(fc / 1000);
        target_[size_t(b)] = float(std::clamp((db - kFloorDb) / -kFloorDb, 0.0, 1.0));
    }
}

void SpectrumView::step()
{
    const int bands = std::clamp(width() / 9, 16, 96);
    const double dt = std::min(0.1, clock_.restart() / 1000.0);
    const bool playing = engine_->state() == Engine::Playing;
    if (playing) {
        rate_ = engine_->scopeSnapshot(samples_.data(), kN);
        analyse(rate_, bands);
    } else target_.assign(size_t(bands), 0.0f);

    if (level_.size() != size_t(bands)) {
        level_.assign(size_t(bands), 0.0f);
        peak_.assign(size_t(bands), 0.0f);
        peakHold_.assign(size_t(bands), 0.0f);
    }
    bool moving = false;
    for (int b = 0; b < bands; ++b) {
        float &l = level_[size_t(b)], &p = peak_[size_t(b)], &h = peakHold_[size_t(b)];
        const float t = target_[size_t(b)];
        l = t > l ? l + (t - l) * 0.7f : std::max(t, l - float(dt * 1.6));   // quick up, gentle fall
        if (l >= p) {
            p = l;
            h = 0.45f;
        } else if ((h -= float(dt)) < 0) p = std::max(l, p - float(dt * 0.7));
        moving |= l > 0 || p > 0;
    }
    update();
    if (!playing && !moving) timer_.stop();   // bars have settled; restarted on the next show
}

QRect SpectrumView::barArea() const
{
    return QRect(0, 4, width(), height() - 16 - 4);   // 16 px for the frequency labels
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
    const QRectF area = barArea();
    const double hi = std::min(kHiHz, rate_ / 2.0 * 0.95);
    auto xOf = [&](double hz) { return area.left() + area.width() * std::log(hz / kLoHz) / std::log(hi / kLoHz); };
    QColor grid = t.muted;
    grid.setAlphaF(0.35);
    for (double hz : {50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0}) {
        const double x = std::round(xOf(hz)) + 0.5;
        p.setPen(QPen(grid, 1, Qt::DotLine));
        p.drawLine(QPointF(x, area.top()), QPointF(x, area.bottom()));
        p.setPen(t.fgDim);
        const QString s = hz >= 1000 ? QString("%1k").arg(hz / 1000) : QString::number(hz);
        p.drawText(QRectF(x - 20, area.bottom() + 2, 40, 16), Qt::AlignHCenter | Qt::AlignTop, s);
    }
    p.setPen(QPen(grid, 1));
    p.drawLine(area.bottomLeft(), area.bottomRight());
}

void SpectrumView::paintEvent(QPaintEvent *)
{
    const Theme &t = Theme::get();
    paintBackground();
    QPainter p(this);
    p.drawPixmap(0, 0, bg_);
    const QRect area = barArea();
    const int bands = int(level_.size());
    if (!bands) {
        p.setPen(t.fgDim);
        p.drawText(area, Qt::AlignCenter, "El espectro aparecerá al reproducir");
        return;
    }
    // plain pixel-aligned rects (no antialiasing): this runs 30 times a second
    const double slot = double(area.width()) / bands;
    const int gap = std::clamp(int(slot * 0.22), 1, 3);
    QLinearGradient g(area.bottomLeft(), area.topLeft());
    QColor base = t.accent;
    base.setAlphaF(0.55);
    g.setColorAt(0, base);
    g.setColorAt(1, t.accent.lighter(t.dark ? 140 : 110));
    const QBrush bars(g);
    for (int b = 0; b < bands; ++b) {
        const int x0 = area.left() + int(std::round(b * slot)) + gap / 2;
        const int x1 = area.left() + int(std::round((b + 1) * slot)) - (gap - gap / 2);
        const int h = int(std::round(level_[size_t(b)] * area.height()));
        if (h > 0) p.fillRect(QRect(x0, area.bottom() + 1 - h, x1 - x0, h), bars);
        if (peak_[size_t(b)] > 0.01) {
            const int y = area.bottom() + 1 - int(std::round(peak_[size_t(b)] * area.height()));
            p.fillRect(QRect(x0, y - 2, x1 - x0, 2), t.fgLight);
        }
    }
}
