#pragma once
#include <memory>
#include <vector>

extern "C" {
#include <libavutil/hwcontext.h>
}

#include "decoder/i_decoder.h"
#include "decoder/soft_decoder.h"
#include "decoder/hw_decoder.h"
#include "common/log.h"

namespace sp {

enum class DecoderType {
    Auto,       // Try hw first, fallback to sw
    Software,   // Force software decoding
    Hardware    // Force hardware decoding (fail if unavailable)
};

class DecoderFactory {
public:
    // Create the best available decoder for the given codec parameters.
    // Returns nullptr only if all options fail.
    static std::unique_ptr<IDecoder> create(
        PacketQueue& pkt_queue,
        FrameQueue& frame_queue,
        const AVCodecParameters* codecpar,
        DecoderType type = DecoderType::Auto,
        const char* tag = "Dec")
    {
        if (type == DecoderType::Software) {
            return create_soft(pkt_queue, frame_queue, codecpar, tag);
        }

        // Try hardware decoders in platform-preferred order
        auto hw_types = get_preferred_hw_types();
        for (auto hw_type : hw_types) {
            auto dec = std::make_unique<HWDecoder>(pkt_queue, frame_queue, hw_type, tag);
            if (dec->open(codecpar)) {
                SP_LOGI("Factory", "Using HW decoder: %s", dec->name());
                return dec;
            }
            SP_LOGW("Factory", "HW decoder %s failed, trying next...",
                    av_hwdevice_get_type_name(hw_type));
        }

        if (type == DecoderType::Hardware) {
            SP_LOGE("Factory", "No hardware decoder available");
            return nullptr;
        }

        // Fallback to software
        SP_LOGI("Factory", "Falling back to software decoder");
        return create_soft(pkt_queue, frame_queue, codecpar, tag);
    }

    // List available hardware decoder types on this system.
    static std::vector<AVHWDeviceType> get_available_hw_types() {
        std::vector<AVHWDeviceType> result;
        AVHWDeviceType type = AV_HWDEVICE_TYPE_NONE;
        while ((type = av_hwdevice_iterate_types(type)) != AV_HWDEVICE_TYPE_NONE) {
            if (HWDecoder::is_available(type)) {
                result.push_back(type);
            }
        }
        return result;
    }

    // Print available decoders to log.
    static void log_available() {
        SP_LOGI("Factory", "Software decoder: always available");
        auto types = get_available_hw_types();
        if (types.empty()) {
            SP_LOGI("Factory", "Hardware decoders: none available");
        } else {
            for (auto t : types) {
                SP_LOGI("Factory", "Hardware decoder available: %s",
                        av_hwdevice_get_type_name(t));
            }
        }
    }

private:
    static std::unique_ptr<IDecoder> create_soft(
        PacketQueue& pkt_queue, FrameQueue& frame_queue,
        const AVCodecParameters* codecpar, const char* tag)
    {
        bool is_video = (codecpar->codec_type == AVMEDIA_TYPE_VIDEO);
        auto dec = std::make_unique<SoftDecoder>(pkt_queue, frame_queue, tag, is_video);
        if (dec->open(codecpar)) return dec;
        return nullptr;
    }

    // Platform-preferred hardware decoder order.
    static std::vector<AVHWDeviceType> get_preferred_hw_types() {
        std::vector<AVHWDeviceType> types;

#if defined(__ANDROID__)
        types.push_back(AV_HWDEVICE_TYPE_MEDIACODEC);
#elif defined(__APPLE__)
        types.push_back(AV_HWDEVICE_TYPE_VIDEOTOOLBOX);
#elif defined(_WIN32)
        types.push_back(AV_HWDEVICE_TYPE_D3D11VA);
        types.push_back(AV_HWDEVICE_TYPE_DXVA2);
#elif defined(__linux__)
        types.push_back(AV_HWDEVICE_TYPE_VAAPI);
        types.push_back(AV_HWDEVICE_TYPE_VDPAU);
#endif
        // CUDA as universal fallback for NVIDIA
        types.push_back(AV_HWDEVICE_TYPE_CUDA);

        return types;
    }
};

} // namespace sp
