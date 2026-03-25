#pragma once
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <cstdio>

#include "common/stats.h"
#include "common/log.h"

namespace sp {

// Debug HUD overlay with TTF vector font rendering.
// Toggle with 'D' key.
class DebugHUD {
public:
    ~DebugHUD() { destroy(); }

    void toggle() { visible_ = !visible_; }
    bool visible() const { return visible_; }

    void render(SDL_Renderer* renderer, const PlayStats& stats) {
        if (!visible_ || !renderer) return;

        // Lazy init TTF
        if (!font_ && !init_failed_) {
            init_ttf();
        }
        if (!font_) return;

        renderer_ = renderer;

        // Semi-transparent background
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        SDL_Rect bg = {10, 10, 400, 280};
        SDL_SetRenderDrawColor(renderer, 15, 15, 25, 210);
        SDL_RenderFillRect(renderer, &bg);

        // Accent bar
        SDL_Rect accent = {10, 10, 400, 3};
        SDL_SetRenderDrawColor(renderer, 0x00, 0xcc, 0xff, 255);
        SDL_RenderFillRect(renderer, &accent);

        int y = 20;
        const int x = 18;
        const int lh = 24;
        char buf[256];

        // Title
        draw_line(x, y, "STREAMING PLAYER DEBUG", {0x00, 0xcc, 0xff, 255}); y += lh;

        // Separator
        y += 4;

        // Decoder
        snprintf(buf, sizeof(buf), "Decoder: %s %s",
                 stats.video_decoder_name,
                 stats.is_hardware_decode ? "[HW]" : "[SW]");
        draw_line(x, y, buf, stats.is_hardware_decode
                  ? SDL_Color{0x00, 0xff, 0x88, 255}
                  : SDL_Color{0xff, 0xcc, 0x00, 255}); y += lh;

        // Codec + resolution
        snprintf(buf, sizeof(buf), "Codec: %s  %dx%d",
                 stats.video_codec, stats.video_width, stats.video_height);
        draw_line(x, y, buf, {0xbb, 0xbb, 0xbb, 255}); y += lh;

        // FPS
        snprintf(buf, sizeof(buf), "FPS: %.1f  Decoded: %ld  Rendered: %ld",
                 stats.fps, (long)stats.video_frames_decoded,
                 (long)stats.video_frames_rendered);
        draw_line(x, y, buf, {0xff, 0xff, 0x44, 255}); y += lh;

        // Dropped / Stutters
        snprintf(buf, sizeof(buf), "Dropped: %ld  Stutters: %d",
                 (long)stats.video_frames_dropped, stats.stutter_count);
        SDL_Color drop_c = stats.video_frames_dropped > 0
            ? SDL_Color{0xff, 0x44, 0x44, 255}
            : SDL_Color{0x88, 0xff, 0x88, 255};
        draw_line(x, y, buf, drop_c); y += lh;

        // Decode time
        snprintf(buf, sizeof(buf), "Decode: avg %.1fms  max %.1fms",
                 stats.avg_decode_time_ms, stats.max_decode_time_ms);
        draw_line(x, y, buf, {0xbb, 0xbb, 0xbb, 255}); y += lh;

        // Buffers
        snprintf(buf, sizeof(buf), "Buf: vpkt=%d apkt=%d vfrm=%d afrm=%d",
                 stats.video_pkt_queue_size, stats.audio_pkt_queue_size,
                 stats.video_frame_queue_size, stats.audio_frame_queue_size);
        draw_line(x, y, buf, {0x44, 0xaa, 0xff, 255}); y += lh;

        // A/V sync
        snprintf(buf, sizeof(buf), "A/V Sync: %.1fms", stats.av_sync_diff_ms);
        SDL_Color sync_c = (stats.av_sync_diff_ms > 50 || stats.av_sync_diff_ms < -50)
            ? SDL_Color{0xff, 0x44, 0x44, 255}
            : SDL_Color{0x88, 0xff, 0x88, 255};
        draw_line(x, y, buf, sync_c); y += lh;

        // First frame
        snprintf(buf, sizeof(buf), "First frame: %.0fms  Rebuf: %d",
                 stats.first_frame_time_ms, stats.buffering_count);
        draw_line(x, y, buf, {0xbb, 0xbb, 0xbb, 255}); y += lh;

        // Footer
        draw_line(x, y + 6, "Press D to hide", {0x66, 0x66, 0x66, 255});
    }

private:
    void init_ttf() {
        if (TTF_WasInit() == 0) {
            if (TTF_Init() < 0) {
                SP_LOGE("HUD", "TTF_Init failed: %s", TTF_GetError());
                init_failed_ = true;
                return;
            }
        }

        // Try common monospace font paths
        static const char* font_paths[] = {
            "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
            "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
            "/usr/share/fonts/truetype/ubuntu/UbuntuMono-R.ttf",
            "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
            "/usr/share/fonts/liberation-mono/LiberationMono-Regular.ttf",
            "/System/Library/Fonts/Menlo.ttc",         // macOS
            "C:\\Windows\\Fonts\\consola.ttf",          // Windows
            nullptr
        };

        for (int i = 0; font_paths[i]; i++) {
            font_ = TTF_OpenFont(font_paths[i], 14);
            if (font_) {
                SP_LOGI("HUD", "Loaded font: %s", font_paths[i]);
                return;
            }
        }

        SP_LOGE("HUD", "No monospace font found, debug HUD text disabled");
        init_failed_ = true;
    }

    void draw_line(int x, int y, const char* text, SDL_Color color) {
        if (!font_ || !renderer_) return;

        SDL_Surface* surface = TTF_RenderUTF8_Blended(font_, text, color);
        if (!surface) return;

        SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer_, surface);
        if (texture) {
            SDL_Rect dst = {x, y, surface->w, surface->h};
            SDL_RenderCopy(renderer_, texture, nullptr, &dst);
            SDL_DestroyTexture(texture);
        }
        SDL_FreeSurface(surface);
    }

    void destroy() {
        if (font_) { TTF_CloseFont(font_); font_ = nullptr; }
    }

    TTF_Font* font_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    bool visible_ = false;
    bool init_failed_ = false;
};

} // namespace sp
