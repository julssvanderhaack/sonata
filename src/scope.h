// Tap for the spectrum view: the realtime callback copies a mono mix of what
// it hands to PipeWire into a small ring the GUI peeks at. Only enabled while
// the view is on screen, so it costs nothing otherwise. The GUI may read a
// sample that is being overwritten; for a picture that is harmless.
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>

class Scope {
public:
    static constexpr size_t kSize = 1 << 14;   // mono frames, ~370 ms at 44.1 kHz

    void setEnabled(bool on) { on_.store(on, std::memory_order_relaxed); }

    // realtime thread
    void push(const float *buf, unsigned frames, int ch)
    {
        if (!on_.load(std::memory_order_relaxed)) return;
        const uint64_t w = w_.load(std::memory_order_relaxed);
        const float k = 1.0f / float(ch);
        for (unsigned i = 0; i < frames; ++i) {
            float s = 0;
            for (int c = 0; c < ch; ++c) s += buf[size_t(i) * ch + c];
            buf_[(w + i) & (kSize - 1)].store(s * k, std::memory_order_relaxed);
        }
        w_.store(w + frames, std::memory_order_release);
    }

    // GUI thread: the n frames ending `lag` frames before the newest one
    // (lag = what is still queued in PipeWire, so the picture matches the sound).
    void snapshot(float *out, size_t n, uint64_t lag) const
    {
        const uint64_t w = w_.load(std::memory_order_acquire);
        lag = std::min<uint64_t>(lag, kSize - n);
        const uint64_t end = w > lag ? w - lag : 0;
        for (size_t i = 0; i < n; ++i) {
            const uint64_t at = end - n + i;   // wraps harmlessly before the first n frames
            out[i] = end >= n || at < end ? buf_[at & (kSize - 1)].load(std::memory_order_relaxed) : 0.0f;
        }
    }

private:
    std::array<std::atomic<float>, kSize> buf_{};
    std::atomic<uint64_t> w_{0};
    std::atomic<bool> on_{false};
};
