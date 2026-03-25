#pragma once
#include <string>
#include <thread>
#include <atomic>
#include <functional>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
}

#include "common/packet_queue.h"
#include "common/log.h"

namespace sp {

struct MediaInfo {
    int video_stream_idx = -1;
    int audio_stream_idx = -1;
    AVCodecParameters* video_codecpar = nullptr;
    AVCodecParameters* audio_codecpar = nullptr;
    AVRational video_time_base{};
    AVRational audio_time_base{};
    double duration = 0.0; // seconds
    bool is_live = false;  // true for live streams (no duration/seek)
};

// Combined source + demuxer: opens a file/URL and demuxes into packet queues.
// Supports local files, RTMP, HTTP-FLV, HLS, DASH.
class Demuxer {
public:
    Demuxer(PacketQueue& video_queue, PacketQueue& audio_queue)
        : video_queue_(video_queue), audio_queue_(audio_queue) {}

    ~Demuxer() { close(); }

    bool open(const std::string& url) {
        url_ = url;
        bool is_network = is_network_url(url);

        AVFormatContext* ctx = avformat_alloc_context();
        if (!ctx) return false;

        // Set interrupt callback for network timeout
        if (is_network) {
            ctx->interrupt_callback.callback = interrupt_cb;
            ctx->interrupt_callback.opaque = this;
        }

        AVDictionary* opts = nullptr;
        if (is_network) {
            // Network-specific options
            av_dict_set(&opts, "timeout", "5000000", 0);      // 5s connect timeout (microseconds)
            av_dict_set(&opts, "reconnect", "1", 0);           // auto reconnect
            av_dict_set(&opts, "reconnect_streamed", "1", 0);
            av_dict_set(&opts, "reconnect_delay_max", "5", 0); // max 5s reconnect delay

            // Protocol-specific tuning
            if (url.find("rtmp://") == 0) {
                av_dict_set(&opts, "live", "1", 0);
            } else if (url.find(".m3u8") != std::string::npos) {
                // HLS: allow longer probe for stream info
                av_dict_set(&opts, "analyzeduration", "5000000", 0);
                av_dict_set(&opts, "probesize", "5000000", 0);
            }

            // Buffer size for network streams
            av_dict_set(&opts, "buffer_size", "1048576", 0);   // 1MB UDP buffer
        }

        last_io_time_ = time(nullptr);
        opening_ = true;
        int ret = avformat_open_input(&ctx, url.c_str(), nullptr, &opts);
        opening_ = false;
        av_dict_free(&opts);

        if (ret < 0) {
            char errbuf[128];
            av_strerror(ret, errbuf, sizeof(errbuf));
            SP_LOGE("Demuxer", "Failed to open %s: %s", url.c_str(), errbuf);
            return false;
        }
        fmt_ctx_ = ctx;

        opening_ = true;
        last_io_time_ = time(nullptr);
        if (avformat_find_stream_info(fmt_ctx_, nullptr) < 0) {
            opening_ = false;
            SP_LOGE("Demuxer", "Failed to find stream info");
            return false;
        }
        opening_ = false;

        // Find best streams
        info_.video_stream_idx = av_find_best_stream(fmt_ctx_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        info_.audio_stream_idx = av_find_best_stream(fmt_ctx_, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);

        if (info_.video_stream_idx >= 0) {
            auto* st = fmt_ctx_->streams[info_.video_stream_idx];
            info_.video_codecpar = st->codecpar;
            info_.video_time_base = st->time_base;
        }
        if (info_.audio_stream_idx >= 0) {
            auto* st = fmt_ctx_->streams[info_.audio_stream_idx];
            info_.audio_codecpar = st->codecpar;
            info_.audio_time_base = st->time_base;
        }

        if (fmt_ctx_->duration > 0) {
            info_.duration = static_cast<double>(fmt_ctx_->duration) / AV_TIME_BASE;
        }

        // Detect live stream
        info_.is_live = is_network && (info_.duration <= 0.0
            || url.find("rtmp://") == 0
            || url.find(".flv") != std::string::npos);

        SP_LOGI("Demuxer", "Opened: video=%d audio=%d duration=%.1fs live=%s url=%s",
                info_.video_stream_idx, info_.audio_stream_idx, info_.duration,
                info_.is_live ? "yes" : "no", url.c_str());
        return true;
    }

    const MediaInfo& media_info() const { return info_; }

    // Start demuxing in a background thread.
    void start() {
        running_ = true;
        thread_ = std::thread(&Demuxer::demux_loop, this);
    }

    void stop() {
        running_ = false;
        video_queue_.abort();
        audio_queue_.abort();
        if (thread_.joinable()) thread_.join();
    }

    // Seek to position in seconds. Not supported for live streams.
    bool seek(double pos_sec) {
        if (!fmt_ctx_ || info_.is_live) return false;
        int64_t ts = static_cast<int64_t>(pos_sec * AV_TIME_BASE);
        int ret = avformat_seek_file(fmt_ctx_, -1, INT64_MIN, ts, INT64_MAX, 0);
        if (ret < 0) {
            SP_LOGE("Demuxer", "Seek failed: %d", ret);
            return false;
        }
        video_queue_.flush();
        audio_queue_.flush();
        return true;
    }

    void set_eof_callback(std::function<void()> cb) { eof_cb_ = std::move(cb); }
    void set_error_callback(std::function<void(int)> cb) { error_cb_ = std::move(cb); }

    void close() {
        stop();
        if (fmt_ctx_) {
            avformat_close_input(&fmt_ctx_);
            fmt_ctx_ = nullptr;
        }
        info_ = {};
    }

private:
    static bool is_network_url(const std::string& url) {
        return url.find("://") != std::string::npos
            && url.find("file://") != 0;
    }

    // Interrupt callback for network timeout (10s no data → abort)
    static int interrupt_cb(void* opaque) {
        auto* self = static_cast<Demuxer*>(opaque);
        // During open() phase, opening_ is true; during demux, running_ is true
        if (!self->opening_ && !self->running_) return 1; // abort if stopped

        time_t now = time(nullptr);
        if (now - self->last_io_time_ > 10) {
            SP_LOGE("Demuxer", "Network timeout (10s no data)");
            return 1;
        }
        return 0;
    }

    void demux_loop() {
        AVPacket* pkt = av_packet_alloc();
        int consecutive_errors = 0;

        while (running_) {
            last_io_time_ = time(nullptr);
            int ret = av_read_frame(fmt_ctx_, pkt);

            if (ret < 0) {
                if (ret == AVERROR_EOF) {
                    if (info_.is_live) {
                        // Live stream EOF: wait and retry
                        SP_LOGW("Demuxer", "Live stream interrupted, retrying...");
                        std::this_thread::sleep_for(std::chrono::milliseconds(500));
                        consecutive_errors++;
                        if (consecutive_errors > 20) { // 10s of retries
                            SP_LOGE("Demuxer", "Live stream reconnect failed");
                            if (error_cb_) error_cb_(ret);
                            break;
                        }
                        continue;
                    }
                    SP_LOGI("Demuxer", "End of stream");
                    if (eof_cb_) eof_cb_();
                    break;
                }

                char errbuf[128];
                av_strerror(ret, errbuf, sizeof(errbuf));
                SP_LOGW("Demuxer", "Read error: %s", errbuf);
                consecutive_errors++;
                if (consecutive_errors > 10) {
                    SP_LOGE("Demuxer", "Too many consecutive errors, stopping");
                    if (error_cb_) error_cb_(ret);
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }

            consecutive_errors = 0;

            if (pkt->stream_index == info_.video_stream_idx) {
                if (!video_queue_.push(pkt)) { av_packet_unref(pkt); break; }
            } else if (pkt->stream_index == info_.audio_stream_idx) {
                if (!audio_queue_.push(pkt)) { av_packet_unref(pkt); break; }
            } else {
                av_packet_unref(pkt);
            }
        }
        av_packet_free(&pkt);
    }

    std::string url_;
    AVFormatContext* fmt_ctx_ = nullptr;
    MediaInfo info_;
    PacketQueue& video_queue_;
    PacketQueue& audio_queue_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> opening_{false};
    std::atomic<time_t> last_io_time_{0};
    std::function<void()> eof_cb_;
    std::function<void(int)> error_cb_;
};

} // namespace sp
