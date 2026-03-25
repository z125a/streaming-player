#pragma once
#include <atomic>
#include <cmath>

namespace sp {

// Master clock for A/V synchronization.
// Typically driven by audio callback timestamps.
class Clock {
public:
    void set(double pts) {
        pts_.store(pts, std::memory_order_release);
    }

    double get() const {
        return pts_.load(std::memory_order_acquire);
    }

    void reset() { pts_.store(NAN, std::memory_order_release); }

    bool valid() const { return !std::isnan(get()); }

private:
    std::atomic<double> pts_{NAN};
};

} // namespace sp
