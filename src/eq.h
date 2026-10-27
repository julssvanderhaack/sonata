// 10-band graphic equaliser + volume + limiter, run inside the PipeWire
// callback. Settings come from the GUI thread through a tiny spinlock the
// realtime side only ever *tries* to take, so audio never waits on the GUI.
#pragma once
#include <array>
#include <atomic>
#include <QString>
#include "scope.h"
#include <QVector>

constexpr int kBands = 10;
inline constexpr std::array<double, kBands> kBandHz = {31, 62, 125, 250, 500, 1000, 2000, 4000, 8000, 16000};
constexpr int kMaxChannels = 8;   // up to 7.1 goes out as it is; PipeWire mixes it down if the speakers have fewer

enum class ReplayGain { Off, Track, Album };

struct EqSettings {
    bool enabled = false;
    float preamp = 0;               // dB, user-set
    bool autoPreamp = true;         // pull the level down by the biggest boost (no clipping)
    std::array<float, kBands> gains{};
    ReplayGain replayGain = ReplayGain::Off;   // applied by the decoder, kept here to be saved with the rest
    bool flat() const;
};

struct EqPreset {
    QString name;
    std::array<float, kBands> gains;
};
const QVector<EqPreset> &builtinPresets();

// Peaking-filter coefficients shared by the DSP and the on-screen curve.
struct BiquadCoef { double b0, b1, b2, a1, a2; };
BiquadCoef peakingCoef(double hz, double gainDb, double rate);
double curveDb(const std::array<float, kBands> &gains, double hz, double rate);   // summed response
double curvePeakDb(const std::array<float, kBands> &gains);

class Dsp {
public:
    // Frames the limiter holds back to see peaks coming (~3 ms); part of the output latency.
    static constexpr unsigned kLookahead = 128;

    // GUI thread
    void setEq(const EqSettings &s);
    void setVolume(float linear) { volume_.store(linear, std::memory_order_relaxed); }
    // Pause fades out over a few ms before the stream stops; resume fades back in.
    void setPaused(bool p) { pauseReq_.store(p, std::memory_order_release); }
    Scope &scope() { return scope_; }
    // Decoder thread, while no stream is running
    void setFormat(int rate, int channels);
    // Realtime thread: how many of `frames` to take from the ring (fewer while
    // fading out for a pause, none once paused), then process the buffer;
    // `jump` = the ring skipped (seek, new track), so old and new audio crossfade.
    unsigned begin(unsigned frames);
    void process(float *buf, unsigned frames, unsigned live, bool jump);

private:
    using Coef = BiquadCoef;
    static constexpr unsigned kFade = 256;        // pause / resume / seek fades (~6 ms)
    static constexpr unsigned kScratch = 8192 * kMaxChannels;
    void rebuild();                         // under lock_
    void runEq(float *buf, unsigned frames, const std::array<Coef, kBands> &k, double (*z)[kMaxChannels][2]) const;
    void limit(float *buf, unsigned frames);
    std::atomic_flag lock_ = ATOMIC_FLAG_INIT;
    bool dirty_ = true, reset_ = false;
    EqSettings pending_;
    int rate_ = 44100, channels_ = 2;

    // realtime-owned copy
    std::array<Coef, kBands> coef_{};
    std::array<Coef, kBands> pendingCoef_{};
    int activeBands_ = 0, pendingActive_ = 0;
    float preampLin_ = 1, pendingPreamp_ = 1;
    int ch_ = 2;                             // channels_ as seen by the realtime side
    double z_[kBands][kMaxChannels][2]{};    // band, channel, state
    double zOld_[kBands][kMaxChannels][2]{};
    float scratch_[kScratch];                // the old EQ's output while crossfading to new settings
    std::atomic<float> volume_{1.0f};
    float curGain_ = 1.0f;

    // limiter: delay line + sliding minimum of the gain each frame needs + release + smoothing
    float delay_[kLookahead][kMaxChannels]{};
    unsigned dpos_ = 0;
    float minVal_[kLookahead + 2]{};
    uint64_t minIdx_[kLookahead + 2]{};
    unsigned minHead_ = 0, minTail_ = 0;
    uint64_t frame_ = 0;
    float release_ = 1, releaseCoef_ = 0.9995f;
    float box_[kLookahead]{};
    unsigned bpos_ = 0, below_ = 0;
    double boxSum_ = kLookahead;

    // fades
    std::atomic<bool> pauseReq_{false};
    enum { Running, FadingOut, Paused } run_ = Running;
    unsigned fadeOut_ = 0, fadeIn_ = kFade, jumpPos_ = kLookahead + kFade;

    Scope scope_;
};
