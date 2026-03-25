extern "C" {
#include <libavformat/avformat.h>
}
#include <SDL2/SDL.h>
#include <cstdio>

#include "player/player.h"

int main(int argc, char* argv[]) {
    printf("streaming-player v0.1.0\n");
    if (argc < 2) {
        printf("Usage: sp_player <file_or_url>\n");
        printf("Controls: SPACE=pause  LEFT/RIGHT=seek  Q/ESC=quit\n");
        return 1;
    }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) < 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    sp::Player player;
    if (!player.open(argv[1])) {
        fprintf(stderr, "Failed to open: %s\n", argv[1]);
        SDL_Quit();
        return 1;
    }

    if (!player.play()) {
        fprintf(stderr, "Failed to start playback\n");
        SDL_Quit();
        return 1;
    }

    // Block on SDL event loop
    player.event_loop();
    player.close();
    SDL_Quit();
    return 0;
}
