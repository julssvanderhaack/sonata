#include "eq.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstring>

bool EqSettings::flat() const
{
    if (preamp != 0) return false;
    for (float g : gains)
        if (g != 0) return false;
    return true;
}

const QVector<EqPreset> &builtinPresets()
{
    static const QVector<EqPreset> p = {
        {"Plano", {0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
        {"Rock", {5, 4, 3, 1, -1, -1, 1, 3, 4, 5}},
        {"Pop", {-1, 1, 3, 4, 3, 0, -1, -1, 1, 2}},
        {"Jazz", {3, 2, 1, 2, -1, -1, 0, 1, 2, 3}},
        {"Clásica", {4, 3, 2, 1, -1, -1, 0, 2, 3, 4}},
        {"Electrónica", {5, 4, 1, 0, -2, 2, 1, 1, 4, 5}},
        {"Hip-hop", {5, 4, 1, 3, -1, -1, 1, -1, 2, 3}},
        {"Acústica", {4, 3, 2, 1, 1, 1, 2, 3, 3, 2}},
        {"Voz", {-2, -3, -2, 1, 4, 4, 3, 1, 0, -2}},
        {"Más graves", {7, 6, 4, 2, 0, 0, 0, 0, 0, 0}},
        {"Más agudos", {0, 0, 0, 0, 0, 1, 2, 4, 6, 7}},
        {"Sonoridad", {6, 4, 1, 0, -2, -1, 0, 1, 4, 5}},
        {"Altavoces portátil", {-6, -3, 2, 3, 1, 0, 0, 1, 2, 1}},
        {"Auriculares", {3, 2, 1, 0, -1, 0, 1, 2, 3, 2}},
        {"Noche (suave)", {-2, -1, 0, 1, 2, 2, 1, -1, -3, -4}},
    };
    return p;
}

static constexpr double kQ = 1.0;   // gentle overlap: smooth curve, no ripple between bands

BiquadCoef peakingCoef(double hz, double g, double rate)
{
    // RBJ cookbook peaking filter
    const double A = std::pow(10.0, g / 40.0);
    const double w0 = 2 * M_PI * std::min(hz, 0.45 * rate) / rate;
    const double alpha = std::sin(w0) / (2 * kQ);
    const double c = std::cos(w0);
    const double a0 = 1 + alpha / A;
    return {(1 + alpha * A) / a0, -2 * c / a0, (1 - alpha * A) / a0, -2 * c / a0, (1 - alpha / A) / a0};
}

double curveDb(const std::array<float, kBands> &gains, double hz, double rate)
{
    const std::complex<double> z = std::polar(1.0, -2 * M_PI * hz / rate);
    double db = 0;
    for (int b = 0; b < kBands; ++b) {
        if (gains[b] == 0) continue;
        const BiquadCoef k = peakingCoef(kBandHz[b], gains[b], rate);
        db += 20 * std::log10(std::abs((k.b0 + k.b1 * z + k.b2 * z * z) / (1.0 + k.a1 * z + k.a2 * z * z)));
    }
    return db;
}

double curvePeakDb(const std::array<float, kBands> &gains)
{
    double peak = 0;
    for (int i = 0; i <= 96; ++i) peak = std::max(peak, curveDb(gains, 20 * std::pow(1000.0, i / 96.0), 48000));
    return peak;
}

void Dsp::setEq(const EqSettings &s)
{
    while (lock_.test_and_set(std::memory_order_acquire)) {}
    pending_ = s;
    rebuild();
    lock_.clear(std::memory_order_release);
}

void Dsp::setFormat(int rate, int channels)
{
    while (lock_.test_and_set(std::memory_order_acquire)) {}
    rate_ = rate;
    channels_ = std::clamp(channels, 1, 2);
    std::memset(z_, 0, sizeof z_);
    rebuild();
    lock_.clear(std::memory_order_release);
}

void Dsp::rebuild()
{
    pendingActive_ = 0;
    if (pending_.enabled) {
        for (int b = 0; b < kBands; ++b) pendingCoef_[b] = peakingCoef(kBandHz[b], pending_.gains[b], rate_);
        pendingActive_ = pending_.flat() ? 0 : kBands;
        // headroom = the real peak of the summed curve, not just the biggest slider
        const float pre = pending_.preamp - (pending_.autoPreamp ? float(curvePeakDb(pending_.gains)) : 0.0f);
        pendingPreamp_ = std::pow(10.0f, pre / 20.0f);
    } else {
        pendingPreamp_ = 1;
    }
    dirty_ = true;
}

void Dsp::process(float *buf, unsigned frames)
{
    if (!lock_.test_and_set(std::memory_order_acquire)) {
        if (dirty_) {
            coef_ = pendingCoef_;
            for (int b = 0; b < kBands; ++b)   // skipped (0 dB) bands restart clean when raised again
                if (coef_[b].b0 == 1 && coef_[b].b1 == coef_[b].a1 && coef_[b].b2 == coef_[b].a2) std::memset(z_[b], 0, sizeof z_[b]);
            if (activeBands_ == 0 && pendingActive_) std::memset(z_, 0, sizeof z_);
            activeBands_ = pendingActive_;
            preampLin_ = pendingPreamp_;
            dirty_ = false;
        }
        lock_.clear(std::memory_order_release);
    }
    const int ch = channels_;

    if (activeBands_) {
        for (int b = 0; b < kBands; ++b) {
            const Coef k = coef_[b];
            // a 0 dB band is exactly the identity (b0 = 1, b1 = a1, b2 = a2): skip it
            if (k.b0 == 1 && k.b1 == k.a1 && k.b2 == k.a2) continue;
            for (int c = 0; c < ch; ++c) {
                double z1 = z_[b][c][0], z2 = z_[b][c][1];
                float *p = buf + c;
                for (unsigned i = 0; i < frames; ++i, p += ch) {
                    const double x = *p;
                    const double y = k.b0 * x + z1;
                    z1 = k.b1 * x - k.a1 * y + z2;
                    z2 = k.b2 * x - k.a2 * y;
                    *p = float(y);
                }
                // flush denormals so silence doesn't cost CPU
                z_[b][c][0] = std::abs(z1) < 1e-25 ? 0 : z1;
                z_[b][c][1] = std::abs(z2) < 1e-25 ? 0 : z2;
            }
        }
    }

    scope_.push(buf, frames, ch);   // after the EQ, before the volume: the view doesn't shrink when you turn it down

    // Volume (cubic curve feels linear to the ear) with a per-buffer ramp, plus
    // a safety clip so a hot EQ never wraps in the DAC.
    const float v = volume_.load(std::memory_order_relaxed);
    const float target = v * v * v * preampLin_;
    const float start = curGain_;
    if (start == 1.0f && target == 1.0f && !activeBands_) return;   // bit-perfect path
    const float step = (target - start) / float(frames ? frames : 1);
    float g = start;
    for (unsigned i = 0; i < frames; ++i) {
        g += step;
        for (int c = 0; c < ch; ++c) {
            float s = buf[i * ch + c] * g;
            buf[i * ch + c] = std::clamp(s, -1.0f, 1.0f);
        }
    }
    curGain_ = target;
}
