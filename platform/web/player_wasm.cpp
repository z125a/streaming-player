// WebAssembly player bridge.
// Exposes C API for JavaScript to control the player core.
// Video frames are rendered via WebGL, audio via Web Audio API.
//
// Build: emcmake cmake + emmake make
// The JS side (index.html) handles canvas rendering and audio playback.

#include <emscripten.h>
#include <cstdint>
#include <cstring>

// Note: In a real build, these would include the actual core headers
// with FFmpeg compiled for wasm. For now, this provides the API skeleton.

extern "C" {

// Opaque player handle
struct WasmPlayer {
    int state;          // 0=idle, 1=playing, 2=paused, 3=stopped
    int width;
    int height;
    double duration;
    double current_time;
    double fps;
    int frames_decoded;
    int frames_dropped;
    char url[512];
};

EMSCRIPTEN_KEEPALIVE
void* player_create() {
    auto* p = new WasmPlayer();
    memset(p, 0, sizeof(WasmPlayer));
    return p;
}

EMSCRIPTEN_KEEPALIVE
int player_open(void* handle, const char* url) {
    auto* p = static_cast<WasmPlayer*>(handle);
    strncpy(p->url, url, sizeof(p->url) - 1);
    p->state = 0;

    // TODO: Initialize FFmpeg (wasm build) demuxer + decoder here
    // For now, signal success
    EM_ASM({
        console.log('[wasm] player_open:', UTF8ToString($0));
    }, url);

    return 1; // success
}

EMSCRIPTEN_KEEPALIVE
void player_play(void* handle) {
    auto* p = static_cast<WasmPlayer*>(handle);
    p->state = 1;

    // TODO: Start demux + decode loop using emscripten_set_main_loop
    // Decoded frames would be passed to JS via EM_ASM for WebGL rendering
    EM_ASM({
        console.log('[wasm] player_play');
        // JS side would start requesting animation frames
        if (window._onPlayerPlay) window._onPlayerPlay();
    });
}

EMSCRIPTEN_KEEPALIVE
void player_stop(void* handle) {
    auto* p = static_cast<WasmPlayer*>(handle);
    p->state = 3;

    EM_ASM({
        console.log('[wasm] player_stop');
        if (window._onPlayerStop) window._onPlayerStop();
    });
}

EMSCRIPTEN_KEEPALIVE
void player_destroy(void* handle) {
    auto* p = static_cast<WasmPlayer*>(handle);
    delete p;
}

// Returns a JSON string with player stats (caller must free)
EMSCRIPTEN_KEEPALIVE
const char* player_get_stats(void* handle) {
    auto* p = static_cast<WasmPlayer*>(handle);
    static char buf[512];
    snprintf(buf, sizeof(buf),
        "{\"state\":%d,\"width\":%d,\"height\":%d,"
        "\"duration\":%.2f,\"currentTime\":%.2f,"
        "\"fps\":%.1f,\"framesDecoded\":%d,\"framesDropped\":%d}",
        p->state, p->width, p->height,
        p->duration, p->current_time,
        p->fps, p->frames_decoded, p->frames_dropped);
    return buf;
}

} // extern "C"
