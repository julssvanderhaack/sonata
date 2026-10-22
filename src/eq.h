// 10-band graphic equaliser + volume, run inside the PipeWire callback.
// Settings come from the GUI thread through a tiny spinlock the realtime side
// only ever *tries* to take, so audio never waits on the GUI.
#pragma once
#include <array>
#include <atomic>
#include <QString>
#include "scope.h"
#include <QVector>

constexpr int kBands = 10;
inline constexpr std::array<double, kBands> kBandHz = {31, 62, 125, 250, 500, 1000, 2000, 4000, 8000, 16000};

struct EqSettings {
    bool enabled = false;
    float preamp = 0;               // dB, user-set
    bool autoPreamp = true;         // pull the level down by the biggest boost (no clipping)
    std::array<float, kBands> gains{};
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
    // GUI thread
    void setEq(const EqSettings &s);
    void setVolume(float linear) { volume_.store(linear, std::memory_order_relaxed); }
    Scope &scope() { return scope_; }
    // Decoder thread, while no stream is running
    void setFormat(int rate, int channels);
    // Realtime thread
    void process(float *buf, unsigned frames);

private:
    using Coef = BiquadCoef;
    void rebuild();                         // under lock_
    std::atomic_flag lock_ = ATOMIC_FLAG_INIT;
    bool dirty_ = true;
    EqSettings pending_;
    int rate_ = 44100, channels_ = 2;

    // realtime-owned copy
    std::array<Coef, kBands> coef_{};
    std::array<Coef, kBands> pendingCoef_{};
    int activeBands_ = 0, pendingActive_ = 0;
    float preampLin_ = 1, pendingPreamp_ = 1;
    double z_[kBands][2][2]{};               // band, channel, state
    std::atomic<float> volume_{1.0f};
    float curGain_ = 1.0f;
    Scope scope_;
};
