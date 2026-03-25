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

// Software decoder using libavcodec.
// Reads from PacketQueue, decodes, and pushes to FrameQueue.
class Decoder {
public:
    Decoder(PacketQueue& pkt_queue, FrameQueue& frame_queue, const char* tag = "Decoder")
        : pkt_queue_(pkt_queue), frame_queue_(frame_queue), tag_(tag) {}

    ~Decoder() { close(); }

    bool open(const AVCodecParameters* codecpar) {
        const AVCodec* codec = avcodec_find_decoder(codecpar->codec_id);
        if (!codec) {
            SP_LOGE(tag_, "Codec not found: %d", codecpar->codec_id);
            return false;
        }

        codec_ctx_ = avcodec_alloc_context3(codec);
        avcodec_parameters_to_context(codec_ctx_, codecpar);

        if (avcodec_open2(codec_ctx_, codec, nullptr) < 0) {
            SP_LOGE(tag_, "Failed to open codec");
            return false;
        }

        SP_LOGI(tag_, "Opened codec: %s", codec->name);
        return true;
    }

    void start() {
        running_ = true;
        thread_ = std::thread(&Decoder::decode_loop, this);
    }

    void stop() {
        running_ = false;
        pkt_queue_.abort();
        frame_queue_.abort();
        if (thread_.joinable()) thread_.join();
    }

    void flush() {
        if (codec_ctx_) avcodec_flush_buffers(codec_ctx_);
    }

    AVCodecContext* codec_ctx() const { return codec_ctx_; }

    void close() {
        stop();
        if (codec_ctx_) {
            avcodec_free_context(&codec_ctx_);
            codec_ctx_ = nullptr;
        }
    }

private:
    void decode_loop() {
        AVPacket* pkt = av_packet_alloc();
        AVFrame* frame = av_frame_alloc();

        while (running_) {
            if (!pkt_queue_.pop(pkt)) break;

            int ret = avcodec_send_packet(codec_ctx_, pkt);
            av_packet_unref(pkt);
            if (ret < 0) {
                SP_LOGW(tag_, "send_packet error: %d", ret);
                continue;
            }

            while (ret >= 0) {
                ret = avcodec_receive_frame(codec_ctx_, frame);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
                if (ret < 0) {
                    SP_LOGE(tag_, "receive_frame error: %d", ret);
                    break;
                }
                if (!frame_queue_.push(frame)) {
                    av_frame_unref(frame);
                    goto done;
                }
            }
        }
    done:
        av_packet_free(&pkt);
        av_frame_free(&frame);
    }

    AVCodecContext* codec_ctx_ = nullptr;
    PacketQueue& pkt_queue_;
    FrameQueue& frame_queue_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    const char* tag_;
};

} // namespace sp
