#pragma once
#include <SDL2/SDL.h>

extern "C" {
#include <libavutil/frame.h>
#include <libswscale/swscale.h>
}

#include "common/log.h"

namespace sp {

// SDL2-based video renderer. Converts frames to YUV420P and renders via texture.
class VideoRender {
public:
    ~VideoRender() { destroy(); }

    bool init(SDL_Window* window, int width, int height) {
        width_ = width;
        height_ = height;

        renderer_ = SDL_CreateRenderer(window, -1,
            SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
        if (!renderer_) {
            SP_LOGE("VideoRender", "SDL_CreateRenderer: %s", SDL_GetError());
            return false;
        }

        texture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_IYUV,
            SDL_TEXTUREACCESS_STREAMING, width, height);
        if (!texture_) {
            SP_LOGE("VideoRender", "SDL_CreateTexture: %s", SDL_GetError());
            return false;
        }

        SP_LOGI("VideoRender", "Initialized %dx%d", width, height);
        return true;
    }

    void render(AVFrame* frame) {
        if (!texture_) return;

        render_count_++;
        // Convert to YUV420P if needed
        if (frame->format != AV_PIX_FMT_YUV420P) {
            if (render_count_ == 1) {
                SP_LOGI("VideoRender", "Converting format %d(%s) → YUV420P via sws_scale",
                        frame->format,
                        av_get_pix_fmt_name(static_cast<AVPixelFormat>(frame->format)));
            }
            if (!ensure_sws(frame->width, frame->height, static_cast<AVPixelFormat>(frame->format)))
                return;
            sws_scale(sws_ctx_, frame->data, frame->linesize, 0, frame->height,
                      tmp_frame_->data, tmp_frame_->linesize);
            frame = tmp_frame_;
        }

        SDL_UpdateYUVTexture(texture_, nullptr,
            frame->data[0], frame->linesize[0],
            frame->data[1], frame->linesize[1],
            frame->data[2], frame->linesize[2]);
        SDL_RenderClear(renderer_);
        SDL_RenderCopy(renderer_, texture_, nullptr, nullptr);
        SDL_RenderPresent(renderer_);
    }

    void destroy() {
        if (texture_) { SDL_DestroyTexture(texture_); texture_ = nullptr; }
        if (renderer_) { SDL_DestroyRenderer(renderer_); renderer_ = nullptr; }
        if (sws_ctx_) { sws_freeContext(sws_ctx_); sws_ctx_ = nullptr; }
        if (tmp_frame_) { av_frame_free(&tmp_frame_); }
    }

private:
    bool ensure_sws(int w, int h, AVPixelFormat fmt) {
        if (sws_ctx_ && sws_src_fmt_ == fmt) return true;
        if (sws_ctx_) sws_freeContext(sws_ctx_);
        sws_ctx_ = sws_getContext(w, h, fmt, width_, height_,
            AV_PIX_FMT_YUV420P, SWS_BILINEAR, nullptr, nullptr, nullptr);
        sws_src_fmt_ = fmt;

        if (!tmp_frame_) {
            tmp_frame_ = av_frame_alloc();
            tmp_frame_->format = AV_PIX_FMT_YUV420P;
            tmp_frame_->width = width_;
            tmp_frame_->height = height_;
            av_frame_get_buffer(tmp_frame_, 0);
        }
        return sws_ctx_ != nullptr;
    }

    SDL_Renderer* renderer_ = nullptr;
    SDL_Texture* texture_ = nullptr;
    SwsContext* sws_ctx_ = nullptr;
    AVPixelFormat sws_src_fmt_ = AV_PIX_FMT_NONE;
    AVFrame* tmp_frame_ = nullptr;
    int width_ = 0, height_ = 0;
    int render_count_ = 0;
};

} // namespace sp
