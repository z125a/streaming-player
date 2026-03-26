#pragma once
#include <atomic>
#include <functional>
#include <chrono>

#include "common/log.h"

namespace sp {

enum class BufferState {
    Buffering,  // Not enough data, should pause rendering and show spinner
    Ready,      // Enough data, can play normally
    Full        // Buffer at max, should apply backpressure to source
};

// Buffer strategy engine.
// Controls playback based on buffer levels to minimize stutter while
// keeping latency low for live streams.
//
// Usage:
//   1. Call update() periodically with current buffer durations
//   2. Check state() to decide whether to render or wait
//   3. For live: call live_catchup_speed() to get playback speed > 1.0
class BufferStrategy {
public:
    struct Config {
        // --- VOD settings ---
        double start_buffer_sec = 1.0;    // Minimum buffer before initial play
        double rebuffer_sec = 0.5;         // Minimum buffer to exit rebuffering
        double high_water_sec = 5.0;       // Comfortable buffer level
        double max_buffer_sec = 30.0;      // Max buffer before backpressure

        // --- Live settings ---
        double live_start_sec = 0.5;       // Faster start for live
        double live_target_sec = 1.5;      // Target buffer for live
        double live_max_sec = 4.0;         // Max before catchup kicks in
        double live_catchup_threshold = 3.0; // Start catching up above this
        double live_catchup_speed = 1.1;    // Playback speed during catchup (1.1x)
    };

    explicit BufferStrategy(bool is_live = false) : is_live_(is_live) {}

    void set_config(const Config& cfg) { config_ = cfg; }

    // Update buffer levels (in seconds of content buffered).
    // Returns the new buffer state.
    BufferState update(double video_buf_sec, double audio_buf_sec) {
        double buf = std::min(video_buf_sec, audio_buf_sec);
        buffer_level_ = buf;

        BufferState prev = state_;

        switch (state_) {
        case BufferState::Buffering: {
            double threshold = first_play_
                ? (is_live_ ? config_.live_start_sec : config_.start_buffer_sec)
                : config_.rebuffer_sec;
            if (buf >= threshold) {
                state_ = BufferState::Ready;
                if (first_play_) {
                    first_play_ = false;
                    SP_LOGI("Buffer", "Initial buffering complete (%.2fs)", buf);
                } else {
                    rebuffer_count_++;
                    auto now = std::chrono::steady_clock::now();
                    double rebuf_ms = std::chrono::duration<double, std::milli>(
                        now - rebuffer_start_).count();
                    total_rebuffer_ms_ += rebuf_ms;
                    SP_LOGI("Buffer", "Rebuffering ended (%.0fms, count=%d)",
                            rebuf_ms, rebuffer_count_);
                }
            }
            break;
        }
        case BufferState::Ready: {
            double max = is_live_ ? config_.live_max_sec : config_.max_buffer_sec;
            if (buf >= max) {
                state_ = BufferState::Full;
            } else if (buf <= 0.01) {
                // Buffer depleted → rebuffer
                state_ = BufferState::Buffering;
                rebuffer_start_ = std::chrono::steady_clock::now();
                SP_LOGW("Buffer", "Rebuffering started (buf=%.3fs)", buf);
            }
            break;
        }
        case BufferState::Full: {
            double high = is_live_ ? config_.live_target_sec : config_.high_water_sec;
            if (buf < high) {
                state_ = BufferState::Ready;
            }
            break;
        }
        }

        if (state_ != prev && on_state_change_) {
            on_state_change_(state_);
        }

        return state_;
    }

    // For live streams: returns playback speed to catch up when buffer is too large.
    // Returns 1.0 for normal speed, > 1.0 to catch up.
    double live_catchup_speed() const {
        if (!is_live_) return 1.0;
        if (buffer_level_ > config_.live_catchup_threshold) {
            return config_.live_catchup_speed;
        }
        return 1.0;
    }

    // Should the player render frames right now?
    bool should_render() const {
        return state_ != BufferState::Buffering;
    }

    BufferState state() const { return state_; }
    double buffer_level() const { return buffer_level_; }
    int rebuffer_count() const { return rebuffer_count_; }
    double total_rebuffer_ms() const { return total_rebuffer_ms_; }

    void set_on_state_change(std::function<void(BufferState)> cb) {
        on_state_change_ = std::move(cb);
    }

    const char* state_name() const {
        switch (state_) {
        case BufferState::Buffering: return "Buffering";
        case BufferState::Ready:     return "Ready";
        case BufferState::Full:      return "Full";
        }
        return "Unknown";
    }

private:
    Config config_;
    bool is_live_;
    BufferState state_ = BufferState::Buffering;
    double buffer_level_ = 0;
    bool first_play_ = true;
    int rebuffer_count_ = 0;
    double total_rebuffer_ms_ = 0;
    std::chrono::steady_clock::time_point rebuffer_start_;
    std::function<void(BufferState)> on_state_change_;
};

} // namespace sp
