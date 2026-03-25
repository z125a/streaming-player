#pragma once
#include "decoder/i_decoder.h"

namespace sp {

// Software decoder using libavcodec (CPU-based).
class SoftDecoder : public IDecoder {
public:
    SoftDecoder(PacketQueue& pkt_queue, FrameQueue& frame_queue, const char* tag = "SoftDec")
        : IDecoder(pkt_queue, frame_queue, tag) {}

    ~SoftDecoder() override { close(); }

    bool open(const AVCodecParameters* codecpar) override {
        const AVCodec* codec = avcodec_find_decoder(codecpar->codec_id);
        if (!codec) {
            SP_LOGE(tag_, "Codec not found: %d", codecpar->codec_id);
            return false;
        }

        codec_ctx_ = avcodec_alloc_context3(codec);
        avcodec_parameters_to_context(codec_ctx_, codecpar);

        // Enable multi-threaded decoding
        codec_ctx_->thread_count = 0; // auto-detect
        codec_ctx_->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;

        if (avcodec_open2(codec_ctx_, codec, nullptr) < 0) {
            SP_LOGE(tag_, "Failed to open codec");
            return false;
        }

        SP_LOGI(tag_, "Opened software codec: %s (threads=%d)",
                codec->name, codec_ctx_->thread_count);
        return true;
    }

    void flush() override {
        if (codec_ctx_) avcodec_flush_buffers(codec_ctx_);
    }

    AVCodecContext* codec_ctx() const override { return codec_ctx_; }
    const char* name() const override { return "SoftDecoder"; }

    void close() override {
        IDecoder::close();
        if (codec_ctx_) {
            avcodec_free_context(&codec_ctx_);
            codec_ctx_ = nullptr;
        }
    }

protected:
    void decode_loop() override {
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
};

} // namespace sp
