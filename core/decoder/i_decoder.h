#pragma once
#include <thread>
#include <atomic>

extern "C" {
#include <libavcodec/avcodec.h>
}

#include "common/packet_queue.h"
#include "common/frame_queue.h"
#include "common/log.h"

namespace sp {

// Abstract decoder interface.
// Implementations: SoftDecoder (FFmpeg), HWDecoder (hw-accelerated).
class IDecoder {
public:
    IDecoder(PacketQueue& pkt_queue, FrameQueue& frame_queue, const char* tag)
        : pkt_queue_(pkt_queue), frame_queue_(frame_queue), tag_(tag) {}

    virtual ~IDecoder() { stop(); }

    virtual bool open(const AVCodecParameters* codecpar) = 0;

    void start() {
        running_ = true;
        thread_ = std::thread(&IDecoder::decode_loop, this);
    }

    void stop() {
        running_ = false;
        pkt_queue_.abort();
        frame_queue_.abort();
        if (thread_.joinable()) thread_.join();
    }

    virtual void flush() = 0;
    virtual AVCodecContext* codec_ctx() const = 0;
    virtual const char* name() const = 0;

    // Mark that a seek happened — decoder should discard frames until
    // the next keyframe to avoid artifacts.
    void mark_seek() {
        seeking_ = true;
    }

    virtual void close() {
        stop();
    }

protected:
    virtual void decode_loop() = 0;

    // Check if a decoded frame should be discarded after seek.
    // Returns true if the frame is safe to output.
    bool check_seek_frame(const AVFrame* frame) {
        if (!seeking_) return true;

        // After seek, discard frames until we get a keyframe.
        // For video: check key_frame flag.
        // For audio: always accept (audio recovers quickly).
        if (frame->key_frame || !is_video_) {
            if (is_video_) {
                SP_LOGI(tag_, "Seek: found keyframe at pts=%ld, resuming output",
                        (long)frame->pts);
            }
            seeking_ = false;
            return true;
        }
        return false; // Discard this non-keyframe
    }

    PacketQueue& pkt_queue_;
    FrameQueue& frame_queue_;
    std::atomic<bool> running_{false};
    std::atomic<bool> seeking_{false};
    bool is_video_ = true; // Set by subclass
    const char* tag_;

private:
    std::thread thread_;
};

} // namespace sp
