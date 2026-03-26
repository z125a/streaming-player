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
#include "decoder/decoder_factory.h"
#include "render/video_render.h"
#include "render/audio_render.h"
#include "render/debug_hud.h"
#include "common/stats.h"
#include "common/buffer_strategy.h"
#include "render/subtitle_render.h"

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

        // Log available decoders
        DecoderFactory::log_available();

        // Open video decoder (hw preferred, auto fallback to sw)
        if (info.video_codecpar) {
            video_decoder_ = DecoderFactory::create(
                video_pkt_queue_, video_frame_queue_, info.video_codecpar,
                DecoderType::Auto, "VDec");
            if (!video_decoder_) return false;
            video_width_ = info.video_codecpar->width;
            video_height_ = info.video_codecpar->height;
            video_time_base_ = info.video_time_base;
        }

        // Open audio decoder (always software — hw audio decode has no benefit)
        if (info.audio_codecpar) {
            audio_decoder_ = DecoderFactory::create(
                audio_pkt_queue_, audio_frame_queue_, info.audio_codecpar,
                DecoderType::Software, "ADec");
            if (!audio_decoder_) return false;
            audio_time_base_ = info.audio_time_base;
        }

        duration_ = info.duration;
        is_live_ = info.is_live;

        // Auto-detect subtitle file (same name as video, .srt extension)
        auto srt_path = detect_subtitle_file(url);
        if (!srt_path.empty()) {
            subtitle_render_ = std::make_unique<SubtitleRender>();
            if (subtitle_render_->init(30)) {
                subtitle_render_->load_srt(srt_path);
            }
        }

        SP_LOGI("Player", "Prepared: %dx%d, %.1fs, live=%s, subs=%s",
                video_width_, video_height_, duration_, is_live_ ? "yes" : "no",
                (subtitle_render_ && subtitle_render_->has_subtitles()) ? "yes" : "no");
        return true;
    }

    // Load external subtitle file.
    bool load_subtitle(const std::string& path) {
        if (!subtitle_render_) {
            subtitle_render_ = std::make_unique<SubtitleRender>();
            if (!subtitle_render_->init(30)) return false;
        }
        return subtitle_render_->load_srt(path);
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

        // Init video render (on main thread — SDL requires this)
        if (video_decoder_) {
            video_render_ = std::make_unique<VideoRender>();
            if (!video_render_->init(window_, video_width_, video_height_)) return false;
        }

        // Init audio render
        if (audio_decoder_) {
            AVCodecContext* actx = audio_decoder_->codec_ctx();
            audio_render_ = std::make_unique<AudioRender>();
            audio_render_->audio_time_base_ = audio_time_base_;
            if (!audio_render_->init(actx->sample_rate, actx->channels,
                                      actx->sample_fmt,
                                      [this](AVFrame* f) { return audio_frame_queue_.pop(f); }))
                return false;
        }

        // Init buffer strategy
        buffer_strategy_ = std::make_unique<BufferStrategy>(is_live_);
        buffer_strategy_->set_on_state_change([this](BufferState s) {
            if (s == BufferState::Buffering) stats_.on_buffering_start();
            else stats_.on_buffering_end();
        });

        // Init stats
        stats_.reset();
        stats_.mark_open();
        stats_.set_decoder_info(
            video_decoder_ ? video_decoder_->name() : "none",
            audio_decoder_ ? audio_decoder_->name() : "none",
            video_decoder_ && video_decoder_->codec_ctx()
                ? avcodec_get_name(video_decoder_->codec_ctx()->codec_id) : "none",
            video_width_, video_height_,
            video_decoder_ && std::string(video_decoder_->name()).find("HW") != std::string::npos
        );

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
        if (!demuxer_ || is_live_) return;
        SP_LOGI("Player", "Seek to %.1fs", pos_sec);

        // 1. Flush all queues
        video_pkt_queue_.flush();
        audio_pkt_queue_.flush();
        video_frame_queue_.flush();
        audio_frame_queue_.flush();

        // 2. Flush decoders and mark seek (discard until next keyframe)
        if (video_decoder_) {
            video_decoder_->flush();
            video_decoder_->mark_seek();
        }
        if (audio_decoder_) audio_decoder_->flush();

        // 3. Seek in demuxer
        demuxer_->seek(pos_sec);
        eof_ = false;
    }

    // Run the main loop: SDL events + video rendering on the MAIN THREAD.
    // SDL2 requires all rendering to happen on the thread that created the renderer.
    void event_loop() {
        SDL_Event event;
        AVFrame* frame = av_frame_alloc();

        while (state_ != PlayerState::Stopped) {
            // 1. Process SDL events
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

            // 2. Check EOF
            if (eof_ && video_frame_queue_.size() == 0) {
                SP_LOGI("Player", "Playback finished");
                state_ = PlayerState::Stopped;
                break;
            }

            // 3. Render video frames (on main thread!)
            if (state_ == PlayerState::Playing && video_render_) {
                render_video_frame(frame);
            } else {
                SDL_Delay(10);
            }
        }

        av_frame_free(&frame);
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
        case SDLK_d:
            debug_hud_.toggle();
            SP_LOGI("Player", "Debug HUD: %s", debug_hud_.visible() ? "ON" : "OFF");
            break;
        case SDLK_s:
            subtitles_visible_ = !subtitles_visible_;
            SP_LOGI("Player", "Subtitles: %s", subtitles_visible_ ? "ON" : "OFF");
            break;
        case SDLK_ESCAPE:
        case SDLK_q:
            state_ = PlayerState::Stopped;
            break;
        }
    }

    // Auto-detect .srt file next to video file.
    static std::string detect_subtitle_file(const std::string& url) {
        // Only for local files
        if (url.find("://") != std::string::npos) return "";
        auto dot = url.rfind('.');
        if (dot == std::string::npos) return "";
        std::string srt = url.substr(0, dot) + ".srt";
        FILE* f = fopen(srt.c_str(), "r");
        if (f) { fclose(f); return srt; }
        return "";
    }

    double get_clock() const {
        if (audio_render_) return audio_render_->audio_clock();
        return 0.0;
    }

    // Try to pop a video frame and render it with A/V sync.
    // Called from the main thread event loop — non-blocking.
    void render_video_frame(AVFrame* frame) {
        // Update buffer strategy based on packet queues (larger, better estimate)
        if (buffer_strategy_) {
            // Use packet queue + frame queue combined for buffer estimation
            // Packet durations are in time_base units; use simple heuristic here
            double vpkts = video_pkt_queue_.size();
            double apkts = audio_pkt_queue_.size();
            double vframes = video_frame_queue_.size();
            // Rough estimate: each packet ≈ 1 frame ≈ 33ms at 30fps
            double vbuf = (vpkts + vframes) * 0.033;
            double abuf = (apkts + audio_frame_queue_.size()) * 0.023;
            buffer_strategy_->update(vbuf, abuf);

            if (!buffer_strategy_->should_render()) {
                SDL_Delay(5);
                return;
            }
        }

        // Non-blocking peek: is there a frame ready?
        double peek_pts = 0.0;
        if (!video_frame_queue_.peek_pts(&peek_pts)) {
            // No frame ready yet — don't block the event loop
            SDL_Delay(1);
            return;
        }

        // Compute frame PTS in seconds
        double pts = peek_pts * av_q2d(video_time_base_);

        // A/V sync check
        if (audio_render_) {
            double audio_pts = audio_render_->audio_clock();
            double diff = pts - audio_pts;

            if (diff > 0.05) {
                // Video is ahead — wait, don't pop yet
                SDL_Delay(std::min(static_cast<int>(diff * 1000), 15));
                return;
            }
        }

        // Pop and render
        if (!video_frame_queue_.pop(frame)) return;

        // Re-check sync after pop (for frame dropping)
        if (audio_render_) {
            double actual_pts = 0.0;
            if (frame->pts != AV_NOPTS_VALUE)
                actual_pts = static_cast<double>(frame->pts) * av_q2d(video_time_base_);
            double audio_pts = audio_render_->audio_clock();
            double diff = actual_pts - audio_pts;

            if (diff < -0.1) {
                // Video is behind — drop frame, try next
                if (frame_count_ > 0) {
                    drop_count_++;
                    if (drop_count_ % 30 == 1)
                        SP_LOGW("Player", "Dropping late frame (diff=%.3fs, dropped=%d)",
                                diff, drop_count_);
                }
                av_frame_unref(frame);
                return;
            }
        }

        // Debug: log first frame
        if (frame_count_++ == 0) {
            SP_LOGI("Player", "First video frame rendered: format=%d(%s) %dx%d",
                    frame->format,
                    av_get_pix_fmt_name(static_cast<AVPixelFormat>(frame->format)),
                    frame->width, frame->height);
            stats_.mark_first_frame();
        }

        // Update stats
        stats_.on_video_frame_rendered();
        stats_.set_queue_levels(
            static_cast<int>(video_pkt_queue_.size()),
            static_cast<int>(audio_pkt_queue_.size()),
            static_cast<int>(video_frame_queue_.size()),
            static_cast<int>(audio_frame_queue_.size()));
        if (audio_render_) {
            double actual_pts = (frame->pts != AV_NOPTS_VALUE)
                ? static_cast<double>(frame->pts) * av_q2d(video_time_base_) : 0.0;
            double audio_pts = audio_render_->audio_clock();
            stats_.set_av_sync_diff((actual_pts - audio_pts) * 1000.0);
        }

        // Render video frame (does not present yet)
        video_render_->render(frame);

        // Render subtitles
        if (subtitles_visible_ && subtitle_render_ && subtitle_render_->has_subtitles()) {
            double sub_pts = (frame->pts != AV_NOPTS_VALUE)
                ? static_cast<double>(frame->pts) * av_q2d(video_time_base_) : 0.0;
            int ww, wh;
            SDL_GetWindowSize(window_, &ww, &wh);
            subtitle_render_->render(video_render_->renderer(), sub_pts, ww, wh);
        }

        // Render debug HUD overlay on top of video
        if (debug_hud_.visible()) {
            debug_hud_.render(video_render_->renderer(), stats_.snapshot());
        }

        // Present everything
        video_render_->present();
        av_frame_unref(frame);
    }

    std::string url_;
    std::atomic<PlayerState> state_{PlayerState::Idle};
    double duration_ = 0.0;
    int video_width_ = 0, video_height_ = 0;
    AVRational video_time_base_{1, 1};
    AVRational audio_time_base_{1, 1};
    bool is_live_ = false;
    int frame_count_ = 0;
    int drop_count_ = 0;
    std::atomic<bool> eof_{false};

    // Queues
    PacketQueue video_pkt_queue_{256};
    PacketQueue audio_pkt_queue_{256};
    FrameQueue video_frame_queue_{8};
    FrameQueue audio_frame_queue_{32};

    // Components
    std::unique_ptr<Demuxer> demuxer_;
    std::unique_ptr<IDecoder> video_decoder_;
    std::unique_ptr<IDecoder> audio_decoder_;
    std::unique_ptr<VideoRender> video_render_;
    std::unique_ptr<AudioRender> audio_render_;

    // Subtitles
    std::unique_ptr<SubtitleRender> subtitle_render_;
    bool subtitles_visible_ = true;

    // Buffer strategy
    std::unique_ptr<BufferStrategy> buffer_strategy_;

    // Stats & Debug
    StatsCollector stats_;
    DebugHUD debug_hud_;

    // SDL
    SDL_Window* window_ = nullptr;
};

} // namespace sp
