#include "eq.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstring>
#include <utility>

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
    channels_ = std::clamp(channels, 1, kMaxChannels);
    reset_ = true;   // filter and limiter state belong to the old stream: the realtime side clears them
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

static bool identity(const BiquadCoef &k) { return k.b0 == 1 && k.b1 == k.a1 && k.b2 == k.a2; }

// All the active bands of every channel in one pass over the buffer. Each
// filter's feedback is a chain of dependent multiplies; running band after
// band, channel after channel, the CPU waits on one chain at a time. Side by
// side (state in registers, channels and bands interleaved) it works on
// several at once. Stereo goes further: both channels in one vector register
// and the band count fixed at compile time, so the state never leaves registers.
static void runBands(float *buf, unsigned frames, int ch, const BiquadCoef *k, int nb, double (*s)[kMaxChannels][2])
{
    for (unsigned i = 0; i < frames; ++i, buf += ch)
        for (int c = 0; c < ch; ++c) {
            double x = buf[c];
            for (int b = 0; b < nb; ++b) {
                double *z = s[b][c];
                const double y = k[b].b0 * x + z[0];
                z[0] = k[b].b1 * x - k[b].a1 * y + z[1];
                z[1] = k[b].b2 * x - k[b].a2 * y;
                x = y;
            }
            buf[c] = float(x);
        }
}

typedef double Stereo __attribute__((vector_size(16)));

template <int NB>
static void runStereo(float *buf, unsigned frames, const BiquadCoef *k, double (*s)[kMaxChannels][2])
{
    Stereo b0[NB], b1[NB], b2[NB], a1[NB], a2[NB], z1[NB], z2[NB];
    for (int b = 0; b < NB; ++b) {
        b0[b] = Stereo{k[b].b0, k[b].b0};
        b1[b] = Stereo{k[b].b1, k[b].b1};
        b2[b] = Stereo{k[b].b2, k[b].b2};
        a1[b] = Stereo{k[b].a1, k[b].a1};
        a2[b] = Stereo{k[b].a2, k[b].a2};
        z1[b] = Stereo{s[b][0][0], s[b][1][0]};
        z2[b] = Stereo{s[b][0][1], s[b][1][1]};
    }
    for (unsigned i = 0; i < frames; ++i, buf += 2) {
        Stereo x = {buf[0], buf[1]};
        for (int b = 0; b < NB; ++b) {
            const Stereo y = b0[b] * x + z1[b];
            z1[b] = b1[b] * x - a1[b] * y + z2[b];
            z2[b] = b2[b] * x - a2[b] * y;
            x = y;
        }
        buf[0] = float(x[0]);
        buf[1] = float(x[1]);
    }
    for (int b = 0; b < NB; ++b)
        for (int c = 0; c < 2; ++c) {
            s[b][c][0] = z1[b][c];
            s[b][c][1] = z2[b][c];
        }
}

template <int... NB>
static void dispatchStereo(int nb, float *buf, unsigned frames, const BiquadCoef *k, double (*s)[kMaxChannels][2],
                           std::integer_sequence<int, NB...>)
{
    ((nb == NB + 1 ? runStereo<NB + 1>(buf, frames, k, s) : void()), ...);
}

void Dsp::runEq(float *buf, unsigned frames, const std::array<Coef, kBands> &coef, double (*z)[kMaxChannels][2]) const
{
    const int ch = ch_;
    // a 0 dB band is exactly the identity (b0 = 1, b1 = a1, b2 = a2): skip it
    Coef k[kBands];
    int band[kBands], nb = 0;
    for (int b = 0; b < kBands; ++b)
        if (!identity(coef[b])) {
            k[nb] = coef[b];
            band[nb++] = b;
        }
    if (!nb) return;
    double s[kBands][kMaxChannels][2];
    for (int j = 0; j < nb; ++j) std::memcpy(s[j], z[band[j]], sizeof s[j]);
    if (ch == 2) dispatchStereo(nb, buf, frames, k, s, std::make_integer_sequence<int, kBands>());
    else runBands(buf, frames, ch, k, nb, s);
    // flush denormals so silence doesn't cost CPU
    for (int j = 0; j < nb; ++j)
        for (int c = 0; c < ch; ++c)
            for (double &v : s[j][c]) z[band[j]][c][&v - s[j][c]] = std::abs(v) < 1e-25 ? 0 : v;
}

// Look-ahead limiter with a 0 dBFS ceiling. Each frame asks for the gain that
// keeps it under the ceiling; the smallest request in the look-ahead window,
// released slowly and averaged over the window, reaches that gain exactly
// when the loud frame leaves the delay line, so peaks are turned down
// smoothly instead of being clipped. Below the ceiling the gain is exactly 1:
// samples pass through untouched (only delayed).
void Dsp::limit(float *buf, unsigned frames)
{
    constexpr unsigned kRing = kLookahead + 2, kWindow = kLookahead + 1;
    const int ch = ch_;
    const size_t n = size_t(frames) * ch;

    // Nothing over the ceiling in this buffer nor in the window before it, and
    // fully released: the limiter is only a delay line. Almost all music,
    // almost all the time, and a copy instead of the per-frame work below.
    if (frames >= kLookahead && quiet_ >= kWindow && release_ == 1.0f && !below_) {
        float peak = 0;
        for (size_t k = 0; k < n; ++k) peak = std::max(peak, std::abs(buf[k]));
        if (peak <= 1.0f) {
            float tail[kLookahead][kMaxChannels];
            const size_t keep = size_t(frames - kLookahead) * ch;
            for (unsigned j = 0; j < kLookahead; ++j) std::memcpy(tail[j], buf + keep + size_t(j) * ch, ch * sizeof(float));
            std::memmove(buf + size_t(kLookahead) * ch, buf, keep * sizeof(float));
            for (unsigned j = 0; j < kLookahead; ++j)
                std::memcpy(buf + size_t(j) * ch, delay_[(dpos_ + j) % kLookahead], ch * sizeof(float));
            std::memcpy(delay_, tail, sizeof tail);
            dpos_ = 0;
            frame_ += frames;
            quiet_ += frames;
            // the window holds only "no reduction" now
            minHead_ = 0;
            minTail_ = 1;
            minVal_[0] = 1.0f;
            minIdx_[0] = frame_ - 1;
            boxSum_ = 0;
            return;
        }
    }

    for (unsigned i = 0; i < frames; ++i) {
        float *x = buf + size_t(i) * ch;
        float peak = 0;
        for (int c = 0; c < ch; ++c) peak = std::max(peak, std::abs(x[c]));
        const float req = peak > 1.0f ? 1.0f / peak : 1.0f;
        quiet_ = req < 1.0f ? 0 : quiet_ + 1;

        // sliding minimum (monotonic queue)
        while (minTail_ != minHead_ && minVal_[(minTail_ + kRing - 1) % kRing] >= req) minTail_ = (minTail_ + kRing - 1) % kRing;
        minVal_[minTail_] = req;
        minIdx_[minTail_] = frame_;
        minTail_ = (minTail_ + 1) % kRing;
        while (minIdx_[minHead_] + kWindow <= frame_) minHead_ = (minHead_ + 1) % kRing;
        const float m = minVal_[minHead_];

        release_ = m < release_ ? m : m + (release_ - m) * releaseCoef_;
        if (release_ > 0.99999f) release_ = 1.0f;   // back to exactly 1: the untouched path

        // moving average of the deficit (1 - gain); zero deficit everywhere = gain exactly 1
        const float d = 1.0f - release_;
        if (box_[bpos_] != 0) --below_;
        if (d != 0) ++below_;
        boxSum_ += double(d) - box_[bpos_];
        box_[bpos_] = d;
        bpos_ = (bpos_ + 1) % kLookahead;
        float g = 1.0f;
        if (below_) g = float(1.0 - boxSum_ / kLookahead);
        else boxSum_ = 0;

        float *dl = delay_[dpos_];
        for (int c = 0; c < ch; ++c) {
            const float out = std::clamp(dl[c] * g, -1.0f, 1.0f);
            dl[c] = x[c];
            x[c] = out;
        }
        dpos_ = (dpos_ + 1) % kLookahead;
        ++frame_;
    }
}

unsigned Dsp::begin(unsigned frames)
{
    const bool want = pauseReq_.load(std::memory_order_acquire);
    if (want && run_ == Running) {
        run_ = FadingOut;
        fadeOut_ = 0;
    } else if (!want && run_ != Running) {
        run_ = Running;
        fadeIn_ = 0;
    }
    if (run_ == Paused) return 0;
    if (run_ == FadingOut) return std::min(frames, kFade - fadeOut_);
    return frames;
}

void Dsp::process(float *buf, unsigned frames, unsigned live, bool jump)
{
    bool xfade = false;
    int oldActive = activeBands_;
    std::array<Coef, kBands> oldCoef;
    if (!lock_.test_and_set(std::memory_order_acquire)) {
        if (reset_) {
            ch_ = channels_;
            std::memset(z_, 0, sizeof z_);
            std::memset(delay_, 0, sizeof delay_);
            std::memset(box_, 0, sizeof box_);
            minHead_ = minTail_ = 0;
            below_ = 0;
            quiet_ = 0;
            boxSum_ = 0;
            release_ = 1;
            releaseCoef_ = float(std::exp(-1.0 / (0.08 * rate_)));   // ~80 ms release
            oldActive = 0;
            reset_ = false;
        }
        if (dirty_) {
            oldCoef = coef_;
            std::memcpy(zOld_, z_, sizeof z_);
            coef_ = pendingCoef_;
            for (int b = 0; b < kBands; ++b)   // skipped (0 dB) bands restart clean when raised again
                if (identity(coef_[b])) std::memset(z_[b], 0, sizeof z_[b]);
            if (activeBands_ == 0 && pendingActive_) std::memset(z_, 0, sizeof z_);
            activeBands_ = pendingActive_;
            preampLin_ = pendingPreamp_;
            // moving a slider must not click: this buffer fades from the old EQ to the new one
            xfade = (oldActive || activeBands_) && size_t(live) * ch_ <= kScratch && live > 0;
            dirty_ = false;
        }
        lock_.clear(std::memory_order_release);
    }
    const int ch = ch_;
    const size_t n = size_t(live) * ch;

    if (xfade) {
        std::memcpy(scratch_, buf, n * sizeof(float));
        if (oldActive) runEq(scratch_, live, oldCoef, zOld_);
    }
    if (activeBands_) runEq(buf, live, coef_, z_);
    if (xfade) {
        for (unsigned i = 0; i < live; ++i) {
            const float a = float(i + 1) / float(live);
            for (int c = 0; c < ch; ++c) {
                const size_t k = size_t(i) * ch + c;
                buf[k] = scratch_[k] + (buf[k] - scratch_[k]) * a;
            }
        }
    }

    scope_.push(buf, live, ch);   // after the EQ, before the volume: the view doesn't shrink when you turn it down

    // Volume (cubic curve feels linear to the ear) with a per-buffer ramp.
    const float v = volume_.load(std::memory_order_relaxed);
    const float target = v * v * v * preampLin_;
    const float start = curGain_;
    if (live && !(start == 1.0f && target == 1.0f)) {
        const float step = (target - start) / float(live);
        float g = start;
        for (unsigned i = 0; i < live; ++i) {
            g += step;
            for (int c = 0; c < ch; ++c) buf[size_t(i) * ch + c] *= g;
        }
        curGain_ = target;
    }

    limit(buf, live);

    // fades: pause / resume, and across a seek or a jump to another song the
    // last few ms of the old audio (still in the delay line) fade out and the
    // new audio fades in, instead of a click
    if (jump) jumpPos_ = 0;
    if (run_ == FadingOut || fadeIn_ < kFade || jumpPos_ < kLookahead + kFade) {
        for (unsigned i = 0; i < live; ++i) {
            float g = 1.0f;
            if (run_ == FadingOut) g *= 1.0f - float(std::min(fadeOut_ + i + 1, kFade)) / kFade;
            if (fadeIn_ < kFade) g *= float(fadeIn_++) / kFade;
            if (jumpPos_ < kLookahead) g *= 1.0f - float(jumpPos_) / kLookahead;
            else if (jumpPos_ < kLookahead + kFade) g *= float(jumpPos_ - kLookahead) / kFade;
            if (jumpPos_ < kLookahead + kFade) ++jumpPos_;
            for (int c = 0; c < ch; ++c) buf[size_t(i) * ch + c] *= g;
        }
        if (run_ == FadingOut && (fadeOut_ += live) >= kFade) run_ = Paused;
    }
    if (live < frames) std::memset(buf + n, 0, size_t(frames - live) * ch * sizeof(float));
}
