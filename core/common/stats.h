#pragma once
#include <atomic>
#include <chrono>
#include <mutex>
#include <cstring>

namespace sp {

// Real-time playback quality statistics.
// Thread-safe: updated by decoder/render threads, read by UI/debug panel.
struct PlayStats {
    // Timing
    double first_frame_time_ms = 0;  // Time from open() to first frame rendered
    double buffering_time_ms = 0;    // Total time spent buffering

    // Frame stats
    int64_t video_frames_decoded = 0;
    int64_t video_frames_rendered = 0;
    int64_t video_frames_dropped = 0;
    int64_t audio_frames_decoded = 0;

    // Bitrate
    double video_bitrate_kbps = 0;   // Estimated video bitrate
    double audio_bitrate_kbps = 0;

    // Buffer levels
    int video_pkt_queue_size = 0;
    int audio_pkt_queue_size = 0;
    int video_frame_queue_size = 0;
    int audio_frame_queue_size = 0;

    // Decode performance
    double avg_decode_time_ms = 0;   // Average video decode time per frame
    double max_decode_time_ms = 0;

    // Playback
    double fps = 0;                  // Actual rendered FPS
    double av_sync_diff_ms = 0;      // Current A/V sync difference
    int buffering_count = 0;         // Number of rebuffering events
    int stutter_count = 0;           // Frames where render was late

    // Network (for streaming)
    double download_speed_kbps = 0;

    // Decoder info
    char video_decoder_name[64] = {};
    char audio_decoder_name[64] = {};
    char video_codec[32] = {};
    int video_width = 0;
    int video_height = 0;
    bool is_hardware_decode = false;
};

// Thread-safe stats collector.
class StatsCollector {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    void reset() {
        std::lock_guard<std::mutex> lock(mutex_);
        stats_ = {};
        start_time_ = Clock::now();
        last_fps_time_ = start_time_;
        fps_frame_count_ = 0;
        decode_time_sum_ = 0;
        decode_time_count_ = 0;
        bytes_received_ = 0;
        last_bandwidth_time_ = start_time_;
    }

    void mark_open() {
        start_time_ = Clock::now();
    }

    void mark_first_frame() {
        auto now = Clock::now();
        std::lock_guard<std::mutex> lock(mutex_);
        stats_.first_frame_time_ms =
            std::chrono::duration<double, std::milli>(now - start_time_).count();
    }

    void on_video_frame_decoded() {
        std::lock_guard<std::mutex> lock(mutex_);
        stats_.video_frames_decoded++;
    }

    void on_video_frame_rendered() {
        std::lock_guard<std::mutex> lock(mutex_);
        stats_.video_frames_rendered++;
        fps_frame_count_++;

        // Calculate FPS every second
        auto now = Clock::now();
        double elapsed = std::chrono::duration<double>(now - last_fps_time_).count();
        if (elapsed >= 1.0) {
            stats_.fps = fps_frame_count_ / elapsed;
            fps_frame_count_ = 0;
            last_fps_time_ = now;
        }
    }

    void on_video_frame_dropped() {
        std::lock_guard<std::mutex> lock(mutex_);
        stats_.video_frames_dropped++;
    }

    void on_audio_frame_decoded() {
        std::lock_guard<std::mutex> lock(mutex_);
        stats_.audio_frames_decoded++;
    }

    void on_decode_time(double ms) {
        std::lock_guard<std::mutex> lock(mutex_);
        decode_time_sum_ += ms;
        decode_time_count_++;
        stats_.avg_decode_time_ms = decode_time_sum_ / decode_time_count_;
        if (ms > stats_.max_decode_time_ms)
            stats_.max_decode_time_ms = ms;
    }

    void on_buffering_start() {
        std::lock_guard<std::mutex> lock(mutex_);
        stats_.buffering_count++;
        buffering_start_ = Clock::now();
    }

    void on_buffering_end() {
        auto now = Clock::now();
        std::lock_guard<std::mutex> lock(mutex_);
        stats_.buffering_time_ms +=
            std::chrono::duration<double, std::milli>(now - buffering_start_).count();
    }

    void on_stutter() {
        std::lock_guard<std::mutex> lock(mutex_);
        stats_.stutter_count++;
    }

    void on_bytes_received(int64_t bytes) {
        std::lock_guard<std::mutex> lock(mutex_);
        bytes_received_ += bytes;
        auto now = Clock::now();
        double elapsed = std::chrono::duration<double>(now - last_bandwidth_time_).count();
        if (elapsed >= 1.0) {
            stats_.download_speed_kbps = (bytes_received_ * 8.0 / 1000.0) / elapsed;
            bytes_received_ = 0;
            last_bandwidth_time_ = now;
        }
    }

    void set_av_sync_diff(double diff_ms) {
        std::lock_guard<std::mutex> lock(mutex_);
        stats_.av_sync_diff_ms = diff_ms;
    }

    void set_queue_levels(int vpkt, int apkt, int vframe, int aframe) {
        std::lock_guard<std::mutex> lock(mutex_);
        stats_.video_pkt_queue_size = vpkt;
        stats_.audio_pkt_queue_size = apkt;
        stats_.video_frame_queue_size = vframe;
        stats_.audio_frame_queue_size = aframe;
    }

    void set_video_bitrate(double kbps) {
        std::lock_guard<std::mutex> lock(mutex_);
        stats_.video_bitrate_kbps = kbps;
    }

    void set_decoder_info(const char* vdec, const char* adec,
                          const char* vcodec, int w, int h, bool hw) {
        std::lock_guard<std::mutex> lock(mutex_);
        strncpy(stats_.video_decoder_name, vdec, sizeof(stats_.video_decoder_name) - 1);
        strncpy(stats_.audio_decoder_name, adec, sizeof(stats_.audio_decoder_name) - 1);
        strncpy(stats_.video_codec, vcodec, sizeof(stats_.video_codec) - 1);
        stats_.video_width = w;
        stats_.video_height = h;
        stats_.is_hardware_decode = hw;
    }

    // Get a snapshot of current stats (thread-safe copy).
    PlayStats snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return stats_;
    }

private:
    mutable std::mutex mutex_;
    PlayStats stats_;
    TimePoint start_time_;
    TimePoint last_fps_time_;
    TimePoint buffering_start_;
    TimePoint last_bandwidth_time_;
    int fps_frame_count_ = 0;
    double decode_time_sum_ = 0;
    int decode_time_count_ = 0;
    int64_t bytes_received_ = 0;
};

} // namespace sp
