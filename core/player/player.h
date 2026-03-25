#pragma once
#include <string>
#include <thread>
#include <atomic>
#include <SDL2/SDL.h>

#include "common/log.h"
#include "common/clock.h"
#include "common/packet_queue.h"
#include "common/frame_queue.h"
#include "demuxer/demuxer.h"
#include "decoder/decoder.h"
#include "render/video_render.h"
#include "render/audio_render.h"

namespace sp {

enum class PlayerState {
    Idle, Preparing, Playing, Paused, Stopped
};

class Player {
public:
    ~Player() { close(); }

    bool open(const std::string& url) {
        state_ = PlayerState::Preparing;
        url_ = url;

        // Open demuxer
        demuxer_ = std::make_unique<Demuxer>(video_pkt_queue_, audio_pkt_queue_);
        if (!demuxer_->open(url)) {
            SP_LOGE("Player", "Failed to open: %s", url.c_str());
            state_ = PlayerState::Idle;
            return false;
        }

        const auto& info = demuxer_->media_info();

        // Open video decoder
        if (info.video_codecpar) {
            video_decoder_ = std::make_unique<Decoder>(video_pkt_queue_, video_frame_queue_, "VDec");
            if (!video_decoder_->open(info.video_codecpar)) return false;
            video_width_ = info.video_codecpar->width;
            video_height_ = info.video_codecpar->height;
            video_time_base_ = info.video_time_base;
        }

        // Open audio decoder
        if (info.audio_codecpar) {
            audio_decoder_ = std::make_unique<Decoder>(audio_pkt_queue_, audio_frame_queue_, "ADec");
            if (!audio_decoder_->open(info.audio_codecpar)) return false;
            audio_time_base_ = info.audio_time_base;
        }

        duration_ = info.duration;
        is_live_ = info.is_live;
        SP_LOGI("Player", "Prepared: %dx%d, %.1fs, live=%s",
                video_width_, video_height_, duration_, is_live_ ? "yes" : "no");
        return true;
    }

    // Initialize SDL window + renderers and start playback.
    bool play() {
        if (state_ == PlayerState::Paused) {
            state_ = PlayerState::Playing;
            if (audio_render_) audio_render_->start();
            return true;
        }

        // Create SDL window
        const char* title = is_live_ ? "Streaming Player [LIVE]" : "Streaming Player";
        window_ = SDL_CreateWindow(title,
            SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
            video_width_ > 0 ? video_width_ : 640,
            video_height_ > 0 ? video_height_ : 480,
            SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
        if (!window_) {
            SP_LOGE("Player", "SDL_CreateWindow: %s", SDL_GetError());
            return false;
        }

        // Init video render
        if (video_decoder_) {
            video_render_ = std::make_unique<VideoRender>();
            if (!video_render_->init(window_, video_width_, video_height_)) return false;
        }

        // Init audio render
        if (audio_decoder_) {
            const auto& info = demuxer_->media_info();
            AVCodecContext* actx = audio_decoder_->codec_ctx();
            audio_render_ = std::make_unique<AudioRender>();
            audio_render_->audio_time_base_ = audio_time_base_;
            if (!audio_render_->init(actx->sample_rate, actx->channels,
                                      actx->sample_fmt,
                                      [this](AVFrame* f) { return audio_frame_queue_.pop(f); }))
                return false;
        }

        // Start pipeline
        demuxer_->set_eof_callback([this] { eof_ = true; });
        demuxer_->set_error_callback([this](int err) {
            SP_LOGE("Player", "Stream error: %d", err);
            eof_ = true;
        });
        demuxer_->start();
        if (video_decoder_) video_decoder_->start();
        if (audio_decoder_) audio_decoder_->start();
        if (audio_render_) audio_render_->start();

        state_ = PlayerState::Playing;

        // Video render loop in a separate thread
        if (video_render_) {
            video_thread_ = std::thread(&Player::video_render_loop, this);
        }

        return true;
    }

    void pause() {
        if (state_ == PlayerState::Playing) {
            state_ = PlayerState::Paused;
            if (audio_render_) audio_render_->pause();
        }
    }

    void toggle_pause() {
        if (state_ == PlayerState::Playing) pause();
        else if (state_ == PlayerState::Paused) play();
    }

    void seek(double pos_sec) {
        if (!demuxer_ || is_live_) return; // No seek for live streams
        // Flush queues
        video_pkt_queue_.flush();
        audio_pkt_queue_.flush();
        video_frame_queue_.flush();
        audio_frame_queue_.flush();
        if (video_decoder_) video_decoder_->flush();
        if (audio_decoder_) audio_decoder_->flush();
        demuxer_->seek(pos_sec);
        eof_ = false;
    }

    // Run the SDL event loop (blocking, call from main thread).
    void event_loop() {
        SDL_Event event;
        while (state_ != PlayerState::Stopped) {
            while (SDL_PollEvent(&event)) {
                switch (event.type) {
                case SDL_QUIT:
                    state_ = PlayerState::Stopped;
                    break;
                case SDL_KEYDOWN:
                    handle_key(event.key.keysym.sym);
                    break;
                }
            }
            SDL_Delay(10);
        }
    }

    void close() {
        state_ = PlayerState::Stopped;

        if (demuxer_) demuxer_->stop();
        if (video_decoder_) video_decoder_->stop();
        if (audio_decoder_) audio_decoder_->stop();

        video_pkt_queue_.abort();
        audio_pkt_queue_.abort();
        video_frame_queue_.abort();
        audio_frame_queue_.abort();

        if (video_thread_.joinable()) video_thread_.join();

        audio_render_.reset();
        video_render_.reset();
        audio_decoder_.reset();
        video_decoder_.reset();
        demuxer_.reset();

        if (window_) { SDL_DestroyWindow(window_); window_ = nullptr; }
    }

    PlayerState state() const { return state_; }
    double duration() const { return duration_; }

private:
    void handle_key(SDL_Keycode key) {
        switch (key) {
        case SDLK_SPACE:
            toggle_pause();
            break;
        case SDLK_RIGHT:
            seek(get_clock() + 10.0);
            break;
        case SDLK_LEFT:
            seek(std::max(0.0, get_clock() - 10.0));
            break;
        case SDLK_ESCAPE:
        case SDLK_q:
            state_ = PlayerState::Stopped;
            break;
        }
    }

    double get_clock() const {
        if (audio_render_) return audio_render_->audio_clock();
        return 0.0;
    }

    // A/V sync: video render loop driven by audio clock.
    void video_render_loop() {
        AVFrame* frame = av_frame_alloc();
        while (state_ != PlayerState::Stopped) {
            if (state_ == PlayerState::Paused) {
                SDL_Delay(10);
                continue;
            }

            if (!video_frame_queue_.pop(frame)) break;

            // Compute frame PTS in seconds
            double pts = 0.0;
            if (frame->pts != AV_NOPTS_VALUE)
                pts = static_cast<double>(frame->pts) * av_q2d(video_time_base_);

            // A/V sync: wait until audio clock catches up
            if (audio_render_) {
                double audio_pts = audio_render_->audio_clock();
                double diff = pts - audio_pts;

                if (diff > 0.01) {
                    // Video is ahead — wait
                    int delay_ms = static_cast<int>(diff * 1000);
                    if (delay_ms > 100) delay_ms = 100; // cap wait
                    SDL_Delay(delay_ms);
                } else if (diff < -0.1) {
                    // Video is behind — drop frame
                    av_frame_unref(frame);
                    continue;
                }
            }

            video_render_->render(frame);
            av_frame_unref(frame);
        }
        av_frame_free(&frame);
    }

    std::string url_;
    std::atomic<PlayerState> state_{PlayerState::Idle};
    double duration_ = 0.0;
    int video_width_ = 0, video_height_ = 0;
    AVRational video_time_base_{1, 1};
    AVRational audio_time_base_{1, 1};
    bool is_live_ = false;
    std::atomic<bool> eof_{false};

    // Queues
    PacketQueue video_pkt_queue_{256};
    PacketQueue audio_pkt_queue_{256};
    FrameQueue video_frame_queue_{8};
    FrameQueue audio_frame_queue_{32};

    // Components
    std::unique_ptr<Demuxer> demuxer_;
    std::unique_ptr<Decoder> video_decoder_;
    std::unique_ptr<Decoder> audio_decoder_;
    std::unique_ptr<VideoRender> video_render_;
    std::unique_ptr<AudioRender> audio_render_;

    // SDL
    SDL_Window* window_ = nullptr;
    std::thread video_thread_;
};

} // namespace sp
