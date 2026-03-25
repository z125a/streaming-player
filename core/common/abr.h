#pragma once
#include <vector>
#include <deque>
#include <chrono>
#include <algorithm>
#include <cmath>

#include "common/log.h"

namespace sp {

struct BitrateLevel {
    int index;
    int bandwidth;      // bits per second
    int width;
    int height;
    std::string url;    // Variant stream URL (for HLS master playlist)
};

// Adaptive Bitrate (ABR) controller.
// Estimates available bandwidth and selects the best quality level.
//
// Algorithm: Bandwidth-based with buffer-aware smoothing.
// - Measures download speed over a sliding window
// - Selects the highest quality that fits within estimated bandwidth
// - Hysteresis: requires sustained improvement before upgrading
// - Emergency downgrade: immediate switch when buffer runs low
class ABRController {
public:
    void set_levels(std::vector<BitrateLevel> levels) {
        levels_ = std::move(levels);
        // Sort by bandwidth ascending
        std::sort(levels_.begin(), levels_.end(),
                  [](const auto& a, const auto& b) { return a.bandwidth < b.bandwidth; });
        for (size_t i = 0; i < levels_.size(); i++)
            levels_[i].index = static_cast<int>(i);
        current_level_ = 0; // Start with lowest quality
        if (!levels_.empty()) {
            SP_LOGI("ABR", "Initialized with %zu levels (%.0f ~ %.0f kbps)",
                    levels_.size(),
                    levels_.front().bandwidth / 1000.0,
                    levels_.back().bandwidth / 1000.0);
        }
    }

    // Record a download sample: bytes downloaded in the given duration.
    void on_segment_downloaded(int64_t bytes, double duration_sec) {
        if (duration_sec <= 0) return;
        double bps = (bytes * 8.0) / duration_sec;

        auto now = std::chrono::steady_clock::now();
        samples_.push_back({bps, now});

        // Remove samples older than the window
        auto cutoff = now - window_;
        while (!samples_.empty() && samples_.front().time < cutoff)
            samples_.pop_front();

        estimated_bandwidth_ = compute_ewma();
    }

    // Report current buffer level in seconds.
    void set_buffer_level(double seconds) {
        buffer_level_ = seconds;
    }

    // Get the recommended quality level index.
    // Call this before fetching each new segment.
    int recommend() {
        if (levels_.size() <= 1) return 0;

        int target = find_target_level();

        // Hysteresis: only upgrade if sustained for N samples
        if (target > current_level_) {
            upgrade_count_++;
            if (upgrade_count_ < upgrade_threshold_) {
                return current_level_; // Not yet sustained
            }
        } else {
            upgrade_count_ = 0;
        }

        // Emergency downgrade: buffer critically low
        if (buffer_level_ < emergency_buffer_sec_ && target > 0) {
            target = std::max(0, current_level_ - 1);
            SP_LOGW("ABR", "Emergency downgrade: buffer=%.1fs → level %d", buffer_level_, target);
        }

        if (target != current_level_) {
            SP_LOGI("ABR", "Switch: level %d → %d (%.0f kbps, est bw=%.0f kbps, buf=%.1fs)",
                    current_level_, target,
                    levels_[target].bandwidth / 1000.0,
                    estimated_bandwidth_ / 1000.0,
                    buffer_level_);
            current_level_ = target;
            upgrade_count_ = 0;
        }

        return current_level_;
    }

    int current_level() const { return current_level_; }
    double estimated_bandwidth() const { return estimated_bandwidth_; }
    const std::vector<BitrateLevel>& levels() const { return levels_; }

    const BitrateLevel* current() const {
        if (current_level_ >= 0 && current_level_ < static_cast<int>(levels_.size()))
            return &levels_[current_level_];
        return nullptr;
    }

private:
    int find_target_level() {
        // Select highest level whose bandwidth fits in safety_factor * estimated
        double safe_bw = estimated_bandwidth_ * safety_factor_;
        int target = 0;
        for (int i = static_cast<int>(levels_.size()) - 1; i >= 0; i--) {
            if (levels_[i].bandwidth <= safe_bw) {
                target = i;
                break;
            }
        }
        return target;
    }

    // Exponentially Weighted Moving Average of bandwidth samples.
    double compute_ewma() {
        if (samples_.empty()) return 0;
        double ewma = samples_.front().bps;
        for (size_t i = 1; i < samples_.size(); i++) {
            ewma = alpha_ * samples_[i].bps + (1.0 - alpha_) * ewma;
        }
        return ewma;
    }

    struct Sample {
        double bps;
        std::chrono::steady_clock::time_point time;
    };

    std::vector<BitrateLevel> levels_;
    std::deque<Sample> samples_;
    int current_level_ = 0;
    double estimated_bandwidth_ = 0;
    double buffer_level_ = 0;       // seconds

    // Tuning parameters
    double safety_factor_ = 0.7;    // Use 70% of estimated bandwidth
    double alpha_ = 0.3;            // EWMA smoothing factor (higher = more recent weight)
    int upgrade_threshold_ = 3;     // Require 3 consecutive upgrade signals
    double emergency_buffer_sec_ = 2.0; // Emergency downgrade below 2s buffer

    int upgrade_count_ = 0;
    std::chrono::seconds window_{30}; // Bandwidth estimation window
};

} // namespace sp
