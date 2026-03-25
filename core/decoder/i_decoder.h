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
// Implementations: SoftDecoder (FFmpeg), MediaCodecDecoder (Android), VTDecoder (iOS).
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

    virtual void close() {
        stop();
    }

protected:
    virtual void decode_loop() = 0;

    PacketQueue& pkt_queue_;
    FrameQueue& frame_queue_;
    std::atomic<bool> running_{false};
    const char* tag_;

private:
    std::thread thread_;
};

} // namespace sp
