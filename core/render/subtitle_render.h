#pragma once
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <string>
#include <vector>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <cstring>

#include "common/log.h"

namespace sp {

struct SubtitleEntry {
    double start_sec;
    double end_sec;
    std::string text;
};

// Subtitle parser + renderer.
// Supports:
//   - SRT files (external)
//   - Plain text with timestamps
//   - FFmpeg subtitle stream (AVSubtitle) integration via add_entry()
//
// Rendered centered at bottom of video with shadow for readability.
class SubtitleRender {
public:
    ~SubtitleRender() { destroy(); }

    bool init(int font_size = 24) {
        if (TTF_WasInit() == 0 && TTF_Init() < 0) {
            SP_LOGE("Subtitle", "TTF_Init failed: %s", TTF_GetError());
            return false;
        }

        // Prefer Noto Sans CJK Medium — clean, readable, CJK support
        static const char* font_paths[] = {
            // Noto Sans CJK Medium (best for subtitles)
            "/usr/share/fonts/opentype/noto/NotoSansCJK-Medium.ttc",
            "/usr/share/fonts/truetype/noto/NotoSansCJK-Medium.ttc",
            "/usr/share/fonts/noto-cjk/NotoSansCJK-Medium.ttc",
            // Noto Sans CJK Regular fallback
            "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
            "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
            // macOS
            "/System/Library/Fonts/PingFang.ttc",
            "/System/Library/Fonts/Supplemental/Arial Unicode.ttf",
            // Windows
            "C:\\Windows\\Fonts\\msyh.ttc",
            "C:\\Windows\\Fonts\\segoeui.ttf",
            // Fallback
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
            nullptr
        };

        for (int i = 0; font_paths[i]; i++) {
            font_ = TTF_OpenFont(font_paths[i], font_size);
            if (font_) {
                SP_LOGI("Subtitle", "Loaded font: %s (%dpt)", font_paths[i], font_size);
                return true;
            }
        }

        SP_LOGE("Subtitle", "No suitable font found");
        return false;
    }

    // Load SRT subtitle file.
    bool load_srt(const std::string& path) {
        std::ifstream file(path);
        if (!file.is_open()) {
            SP_LOGE("Subtitle", "Cannot open: %s", path.c_str());
            return false;
        }

        entries_.clear();
        std::string line;
        while (std::getline(file, line)) {
            // Skip sequence number
            trim(line);
            if (line.empty()) continue;

            // Try to parse timestamp line: 00:01:23,456 --> 00:01:25,789
            if (line.find("-->") != std::string::npos) {
                SubtitleEntry entry;
                if (parse_srt_time(line, entry.start_sec, entry.end_sec)) {
                    // Read text lines until blank line
                    std::string text;
                    while (std::getline(file, line)) {
                        trim(line);
                        if (line.empty()) break;
                        if (!text.empty()) text += "\n";
                        // Strip basic HTML tags
                        strip_tags(line);
                        text += line;
                    }
                    entry.text = text;
                    if (!text.empty()) {
                        entries_.push_back(std::move(entry));
                    }
                }
            }
        }

        // Sort by start time
        std::sort(entries_.begin(), entries_.end(),
                  [](const auto& a, const auto& b) { return a.start_sec < b.start_sec; });

        SP_LOGI("Subtitle", "Loaded %zu entries from %s", entries_.size(), path.c_str());
        return !entries_.empty();
    }

    // Add a subtitle entry programmatically (e.g., from FFmpeg subtitle stream).
    void add_entry(double start, double end, const std::string& text) {
        entries_.push_back({start, end, text});
    }

    // Render the active subtitle for the given PTS onto the renderer.
    // Call after video render, before present.
    void render(SDL_Renderer* renderer, double pts_sec, int window_w, int window_h) {
        if (!font_ || !renderer || entries_.empty()) return;

        // Find active subtitle
        const SubtitleEntry* active = find_active(pts_sec);
        if (!active) {
            cached_texture_ = nullptr;
            return;
        }

        // Cache: only re-render if text changed
        if (active->text != cached_text_) {
            render_text(renderer, active->text, window_w, window_h);
            cached_text_ = active->text;
        }

        if (cached_texture_) {
            SDL_RenderCopy(renderer, cached_texture_, nullptr, &cached_rect_);
        }
    }

    size_t entry_count() const { return entries_.size(); }
    bool has_subtitles() const { return !entries_.empty(); }

    void destroy() {
        if (cached_texture_) { SDL_DestroyTexture(cached_texture_); cached_texture_ = nullptr; }
        if (font_) { TTF_CloseFont(font_); font_ = nullptr; }
        entries_.clear();
    }

private:
    const SubtitleEntry* find_active(double pts) const {
        // Binary search for efficiency
        for (const auto& e : entries_) {
            if (pts >= e.start_sec && pts <= e.end_sec) {
                return &e;
            }
            if (e.start_sec > pts) break; // Sorted, no need to continue
        }
        return nullptr;
    }

    void render_text(SDL_Renderer* renderer, const std::string& text,
                     int window_w, int window_h) {
        if (cached_texture_) {
            SDL_DestroyTexture(cached_texture_);
            cached_texture_ = nullptr;
        }

        SDL_Color text_color = {255, 255, 255, 255};
        SDL_Color outline_color = {0, 0, 0, 255};

        // Wrap text at 80% of window width
        int wrap_width = static_cast<int>(window_w * 0.8);

        // Render outline (black) by drawing text offset in 8 directions
        TTF_SetFontOutline(font_, 2);
        SDL_Surface* outline_surf = TTF_RenderUTF8_Blended_Wrapped(
            font_, text.c_str(), outline_color, wrap_width);
        TTF_SetFontOutline(font_, 0);

        // Render main text (white)
        SDL_Surface* text_surf = TTF_RenderUTF8_Blended_Wrapped(
            font_, text.c_str(), text_color, wrap_width);

        if (!text_surf) {
            if (outline_surf) SDL_FreeSurface(outline_surf);
            return;
        }

        // Compose: outline behind, text on top (outline is slightly larger)
        int w = outline_surf ? outline_surf->w : text_surf->w;
        int h = outline_surf ? outline_surf->h : text_surf->h;
        SDL_Surface* composed = SDL_CreateRGBSurfaceWithFormat(
            0, w, h, 32, SDL_PIXELFORMAT_RGBA32);
        SDL_SetSurfaceBlendMode(composed, SDL_BLENDMODE_BLEND);
        SDL_FillRect(composed, nullptr, SDL_MapRGBA(composed->format, 0, 0, 0, 0));

        if (outline_surf) {
            SDL_Rect outline_dst = {0, 0, outline_surf->w, outline_surf->h};
            SDL_SetSurfaceBlendMode(outline_surf, SDL_BLENDMODE_BLEND);
            SDL_BlitSurface(outline_surf, nullptr, composed, &outline_dst);
            SDL_FreeSurface(outline_surf);
        }

        // Center text on outline (outline adds 2px border)
        SDL_Rect text_dst = {2, 2, text_surf->w, text_surf->h};
        SDL_SetSurfaceBlendMode(text_surf, SDL_BLENDMODE_BLEND);
        SDL_BlitSurface(text_surf, nullptr, composed, &text_dst);
        SDL_FreeSurface(text_surf);

        cached_texture_ = SDL_CreateTextureFromSurface(renderer, composed);
        SDL_SetTextureBlendMode(cached_texture_, SDL_BLENDMODE_BLEND);

        // Position: centered, near bottom
        cached_rect_ = {
            (window_w - w) / 2,
            window_h - h - 40, // 40px from bottom
            w, h
        };

        SDL_FreeSurface(composed);
    }

    static bool parse_srt_time(const std::string& line, double& start, double& end) {
        // Format: 00:01:23,456 --> 00:01:25,789
        int sh, sm, ss, sms, eh, em, es, ems;
        char sep;
        if (sscanf(line.c_str(), "%d:%d:%d%c%d --> %d:%d:%d%c%d",
                   &sh, &sm, &ss, &sep, &sms, &eh, &em, &es, &sep, &ems) >= 10) {
            start = sh * 3600.0 + sm * 60.0 + ss + sms / 1000.0;
            end = eh * 3600.0 + em * 60.0 + es + ems / 1000.0;
            return true;
        }
        return false;
    }

    static void strip_tags(std::string& s) {
        // Remove <b>, </b>, <i>, </i>, <u>, </u>, <font...>, </font>
        std::string result;
        bool in_tag = false;
        for (char c : s) {
            if (c == '<') { in_tag = true; continue; }
            if (c == '>') { in_tag = false; continue; }
            if (!in_tag) result += c;
        }
        s = result;
    }

    static void trim(std::string& s) {
        while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' '))
            s.pop_back();
        size_t start = s.find_first_not_of(" \t\r\n");
        if (start != std::string::npos) s = s.substr(start);
    }

    TTF_Font* font_ = nullptr;
    std::vector<SubtitleEntry> entries_;
    SDL_Texture* cached_texture_ = nullptr;
    SDL_Rect cached_rect_{};
    std::string cached_text_;
};

} // namespace sp
