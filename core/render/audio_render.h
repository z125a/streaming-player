#pragma once
#include <SDL2/SDL.h>
#include <functional>

extern "C" {
#include <libavutil/frame.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
}

#include "common/log.h"

namespace sp {

// SDL2-based audio renderer with resampling to SDL's format.
// Uses a pull-based callback model: SDL calls us when it needs more data.
class AudioRender {
public:
    // Callback type: called when audio needs a frame. Returns false if no frame available.
    using FrameCallback = std::function<bool(AVFrame* frame)>;

    ~AudioRender() { destroy(); }

    bool init(int sample_rate, int channels, AVSampleFormat src_fmt, const FrameCallback& cb) {
        frame_cb_ = cb;
        src_sample_rate_ = sample_rate;
        src_channels_ = channels;
        src_fmt_ = src_fmt;

        SDL_AudioSpec wanted{};
        wanted.freq = sample_rate;
        wanted.format = AUDIO_S16SYS;
        wanted.channels = static_cast<uint8_t>(channels);
        wanted.samples = 1024;
        wanted.callback = sdl_audio_callback;
        wanted.userdata = this;

        SDL_AudioSpec obtained{};
        dev_ = SDL_OpenAudioDevice(nullptr, 0, &wanted, &obtained, 0);
        if (dev_ == 0) {
            SP_LOGE("AudioRender", "SDL_OpenAudioDevice: %s", SDL_GetError());
            return false;
        }

        dst_sample_rate_ = obtained.freq;
        dst_channels_ = obtained.channels;

        // Setup resampler
        swr_ctx_ = swr_alloc();
        av_opt_set_int(swr_ctx_, "in_channel_count", src_channels_, 0);
        av_opt_set_int(swr_ctx_, "in_sample_rate", src_sample_rate_, 0);
        av_opt_set_sample_fmt(swr_ctx_, "in_sample_fmt", src_fmt_, 0);
        av_opt_set_int(swr_ctx_, "out_channel_count", dst_channels_, 0);
        av_opt_set_int(swr_ctx_, "out_sample_rate", dst_sample_rate_, 0);
        av_opt_set_sample_fmt(swr_ctx_, "out_sample_fmt", AV_SAMPLE_FMT_S16, 0);

        if (swr_init(swr_ctx_) < 0) {
            SP_LOGE("AudioRender", "swr_init failed");
            return false;
        }

        SP_LOGI("AudioRender", "Initialized: %dHz %dch", dst_sample_rate_, dst_channels_);
        return true;
    }

    void start() {
        if (dev_) SDL_PauseAudioDevice(dev_, 0);
    }

    void pause() {
        if (dev_) SDL_PauseAudioDevice(dev_, 1);
    }

    // Returns the current audio clock (in seconds, based on samples played).
    double audio_clock() const { return audio_clock_; }

    void destroy() {
        if (dev_) { SDL_CloseAudioDevice(dev_); dev_ = 0; }
        if (swr_ctx_) { swr_free(&swr_ctx_); swr_ctx_ = nullptr; }
        if (audio_buf_) { av_free(audio_buf_); audio_buf_ = nullptr; }
    }

private:
    static void sdl_audio_callback(void* userdata, uint8_t* stream, int len) {
        auto* self = static_cast<AudioRender*>(userdata);
        self->fill_audio(stream, len);
    }

    void fill_audio(uint8_t* stream, int len) {
        int written = 0;
        while (written < len) {
            if (buf_index_ >= buf_size_) {
                // Need more data
                AVFrame* frame = av_frame_alloc();
                if (!frame_cb_ || !frame_cb_(frame)) {
                    av_frame_free(&frame);
                    memset(stream + written, 0, len - written);
                    return;
                }

                // Update audio clock from frame PTS
                if (frame->pts != AV_NOPTS_VALUE) {
                    audio_clock_ = static_cast<double>(frame->pts) * av_q2d(audio_time_base_);
                }

                // Resample
                int out_samples = swr_get_out_samples(swr_ctx_, frame->nb_samples);
                if (audio_buf_) av_free(audio_buf_);
                int buf_bytes = out_samples * dst_channels_ * 2; // S16 = 2 bytes
                audio_buf_ = static_cast<uint8_t*>(av_malloc(buf_bytes));

                uint8_t* out[] = {audio_buf_};
                int converted = swr_convert(swr_ctx_, out, out_samples,
                    const_cast<const uint8_t**>(frame->data), frame->nb_samples);

                buf_size_ = converted * dst_channels_ * 2;
                buf_index_ = 0;
                av_frame_free(&frame);
            }

            int remain = buf_size_ - buf_index_;
            int to_copy = std::min(remain, len - written);
            memcpy(stream + written, audio_buf_ + buf_index_, to_copy);
            buf_index_ += to_copy;
            written += to_copy;
        }

        // Update clock based on bytes consumed
        audio_clock_ += static_cast<double>(written) / (dst_sample_rate_ * dst_channels_ * 2);
    }

    SDL_AudioDeviceID dev_ = 0;
    SwrContext* swr_ctx_ = nullptr;
    FrameCallback frame_cb_;

    int src_sample_rate_ = 0, src_channels_ = 0;
    AVSampleFormat src_fmt_ = AV_SAMPLE_FMT_NONE;
    int dst_sample_rate_ = 0, dst_channels_ = 0;

    uint8_t* audio_buf_ = nullptr;
    int buf_size_ = 0;
    int buf_index_ = 0;

    double audio_clock_ = 0.0;

public:
    AVRational audio_time_base_{1, 1};
};

} // namespace sp
