// Lock-free single-producer/single-consumer ring of float samples.
// The decoder thread writes, the PipeWire realtime callback reads. Counters
// grow forever (64-bit), so "position in the stream" is just a sample index.
// Flushing (seek, new track) never touches the reader's index: the producer
// publishes a "discard everything before X" mark and the reader jumps to it.
#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>

class Ring {
public:
    explicit Ring(size_t capacityPow2) : buf_(capacityPow2), mask_(capacityPow2 - 1) {}

    size_t capacity() const { return buf_.size(); }

    // ---- producer side ----
    uint64_t writeIndex() const { return w_.load(std::memory_order_relaxed); }
    uint64_t readIndex() const { return std::max(r_.load(std::memory_order_acquire), discard_.load(std::memory_order_acquire)); }
    size_t space() const { return buf_.size() - size_t(writeIndex() - readIndex()); }
    size_t filled() const { return size_t(writeIndex() - readIndex()); }

    size_t write(const float *src, size_t n)
    {
        n = std::min(n, space());
        const uint64_t w = w_.load(std::memory_order_relaxed);
        const size_t at = size_t(w & mask_);
        const size_t first = std::min(n, buf_.size() - at);
        std::memcpy(buf_.data() + at, src, first * sizeof(float));
        std::memcpy(buf_.data(), src + first, (n - first) * sizeof(float));
        w_.store(w + n, std::memory_order_release);
        return n;
    }
    // Drop everything queued so far; returns the index new data will start at.
    uint64_t flush()
    {
        const uint64_t w = w_.load(std::memory_order_relaxed);
        discard_.store(w, std::memory_order_release);
        return w;
    }

    // ---- consumer side (realtime) ----
    size_t read(float *dst, size_t n)
    {
        uint64_t r = r_.load(std::memory_order_relaxed);
        const uint64_t d = discard_.load(std::memory_order_acquire);
        if (d > r) r = d;
        const uint64_t w = w_.load(std::memory_order_acquire);
        n = std::min<size_t>(n, size_t(w - r));
        const size_t at = size_t(r & mask_);
        const size_t first = std::min(n, buf_.size() - at);
        std::memcpy(dst, buf_.data() + at, first * sizeof(float));
        std::memcpy(dst + first, buf_.data(), (n - first) * sizeof(float));
        r_.store(r + n, std::memory_order_release);
        return n;
    }

private:
    std::vector<float> buf_;
    const size_t mask_;
    alignas(64) std::atomic<uint64_t> w_{0};
    alignas(64) std::atomic<uint64_t> r_{0};
    alignas(64) std::atomic<uint64_t> discard_{0};
};
