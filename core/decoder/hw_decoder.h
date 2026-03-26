#pragma once
#include "decoder/i_decoder.h"

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
}

namespace sp {

// Hardware-accelerated decoder using FFmpeg's hw_device_ctx.
// Supports: MediaCodec (Android), VideoToolbox (iOS/macOS), VAAPI (Linux), D3D11VA (Windows).
//
// The decoded frames are in hardware pixel format and need to be transferred
// to system memory (or rendered directly via hw surface).
class HWDecoder : public IDecoder {
public:
    HWDecoder(PacketQueue& pkt_queue, FrameQueue& frame_queue,
              AVHWDeviceType hw_type, const char* tag = "HWDec")
        : IDecoder(pkt_queue, frame_queue, tag), hw_type_(hw_type) { is_video_ = true; }

    ~HWDecoder() override { close(); }

    bool open(const AVCodecParameters* codecpar) override {
        const AVCodec* codec = avcodec_find_decoder(codecpar->codec_id);
        if (!codec) {
            SP_LOGE(tag_, "Codec not found: %d", codecpar->codec_id);
            return false;
        }

        // Check if this codec supports the requested hw device type
        hw_pix_fmt_ = find_hw_pix_fmt(codec, hw_type_);
        if (hw_pix_fmt_ == AV_PIX_FMT_NONE) {
            SP_LOGW(tag_, "HW format not supported for codec %s with %s",
                    codec->name, av_hwdevice_get_type_name(hw_type_));
            return false;
        }

        codec_ctx_ = avcodec_alloc_context3(codec);
        avcodec_parameters_to_context(codec_ctx_, codecpar);

        // Create hw device context
        int ret = av_hwdevice_ctx_create(&hw_device_ctx_, hw_type_, nullptr, nullptr, 0);
        if (ret < 0) {
            char errbuf[128];
            av_strerror(ret, errbuf, sizeof(errbuf));
            SP_LOGE(tag_, "Failed to create hw device context: %s", errbuf);
            return false;
        }
        codec_ctx_->hw_device_ctx = av_buffer_ref(hw_device_ctx_);

        // Set get_format callback to select hw pixel format
        codec_ctx_->opaque = this;
        codec_ctx_->get_format = get_hw_format;

        if (avcodec_open2(codec_ctx_, codec, nullptr) < 0) {
            SP_LOGE(tag_, "Failed to open hw codec");
            return false;
        }

        SP_LOGI(tag_, "Opened HW decoder: %s [%s] pix_fmt=%s",
                codec->name, av_hwdevice_get_type_name(hw_type_),
                av_get_pix_fmt_name(hw_pix_fmt_));
        return true;
    }

    void flush() override {
        if (codec_ctx_) avcodec_flush_buffers(codec_ctx_);
    }

    AVCodecContext* codec_ctx() const override { return codec_ctx_; }

    const char* name() const override {
        static thread_local char buf[64];
        snprintf(buf, sizeof(buf), "HWDecoder[%s]", av_hwdevice_get_type_name(hw_type_));
        return buf;
    }

    void close() override {
        IDecoder::close();
        if (codec_ctx_) {
            avcodec_free_context(&codec_ctx_);
            codec_ctx_ = nullptr;
        }
        if (hw_device_ctx_) {
            av_buffer_unref(&hw_device_ctx_);
            hw_device_ctx_ = nullptr;
        }
    }

    // Check if a given hw type is available on this system.
    static bool is_available(AVHWDeviceType type) {
        AVBufferRef* ctx = nullptr;
        int ret = av_hwdevice_ctx_create(&ctx, type, nullptr, nullptr, 0);
        if (ret >= 0) {
            av_buffer_unref(&ctx);
            return true;
        }
        return false;
    }

protected:
    void decode_loop() override {
        AVPacket* pkt = av_packet_alloc();
        AVFrame* hw_frame = av_frame_alloc();
        AVFrame* sw_frame = av_frame_alloc();

        while (running_) {
            if (!pkt_queue_.pop(pkt)) break;

            int ret = avcodec_send_packet(codec_ctx_, pkt);
            av_packet_unref(pkt);
            if (ret < 0) {
                SP_LOGW(tag_, "send_packet error: %d", ret);
                continue;
            }

            while (ret >= 0) {
                ret = avcodec_receive_frame(codec_ctx_, hw_frame);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
                if (ret < 0) {
                    SP_LOGE(tag_, "receive_frame error: %d", ret);
                    break;
                }

                // Transfer from GPU → CPU memory
                if (hw_frame->format == hw_pix_fmt_) {
                    ret = av_hwframe_transfer_data(sw_frame, hw_frame, 0);
                    if (ret < 0) {
                        SP_LOGW(tag_, "hw transfer failed: %d", ret);
                        av_frame_unref(hw_frame);
                        continue;
                    }
                    sw_frame->pts = hw_frame->pts;
                    sw_frame->pkt_dts = hw_frame->pkt_dts;
                    sw_frame->best_effort_timestamp = hw_frame->best_effort_timestamp;
                    sw_frame->key_frame = hw_frame->key_frame;
                    av_frame_unref(hw_frame);

                    // After seek: discard non-keyframes
                    if (!check_seek_frame(sw_frame)) {
                        av_frame_unref(sw_frame);
                        continue;
                    }
                    if (!frame_queue_.push(sw_frame)) {
                        av_frame_unref(sw_frame);
                        goto done;
                    }
                } else {
                    // Already in sw format (fallback path)
                    if (!frame_queue_.push(hw_frame)) {
                        av_frame_unref(hw_frame);
                        goto done;
                    }
                }
            }
        }
    done:
        av_packet_free(&pkt);
        av_frame_free(&hw_frame);
        av_frame_free(&sw_frame);
    }

private:
    static AVPixelFormat find_hw_pix_fmt(const AVCodec* codec, AVHWDeviceType type) {
        for (int i = 0;; i++) {
            const AVCodecHWConfig* config = avcodec_get_hw_config(codec, i);
            if (!config) break;
            if (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX
                && config->device_type == type) {
                return config->pix_fmt;
            }
        }
        return AV_PIX_FMT_NONE;
    }

    static AVPixelFormat get_hw_format(AVCodecContext* ctx, const AVPixelFormat* pix_fmts) {
        auto* self = static_cast<HWDecoder*>(ctx->opaque);
        for (const AVPixelFormat* p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
            if (*p == self->hw_pix_fmt_) return *p;
        }
        SP_LOGW(self->tag_, "Failed to get hw format, falling back to sw");
        return pix_fmts[0];
    }

    AVHWDeviceType hw_type_;
    AVPixelFormat hw_pix_fmt_ = AV_PIX_FMT_NONE;
    AVCodecContext* codec_ctx_ = nullptr;
    AVBufferRef* hw_device_ctx_ = nullptr;
};

} // namespace sp
