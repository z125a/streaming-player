# Build with Emscripten:
#   mkdir build-web && cd build-web
#   emcmake cmake ../platform/web -DCMAKE_BUILD_TYPE=Release
#   emmake make
#
# Produces: sp_player.js + sp_player.wasm
# Serve with any HTTP server and open index.html

cmake_minimum_required(VERSION 3.16)
project(sp_player_web)

set(CMAKE_CXX_STANDARD 17)

# Core sources
set(CORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../core")

add_executable(sp_player
    player_wasm.cpp
)

target_include_directories(sp_player PRIVATE
    ${CORE_DIR}
)

# Emscripten flags
set_target_properties(sp_player PROPERTIES
    SUFFIX ".js"
)

target_link_options(sp_player PRIVATE
    # FFmpeg libraries (must be built for wasm separately)
    # "SHELL:-s USE_PTHREADS=1"  # Enable if FFmpeg built with threads
    "SHELL:-s EXPORTED_FUNCTIONS=['_player_create','_player_open','_player_play','_player_stop','_player_destroy','_player_get_stats','_malloc','_free']"
    "SHELL:-s EXPORTED_RUNTIME_METHODS=['ccall','cwrap','UTF8ToString']"
    "SHELL:-s ALLOW_MEMORY_GROWTH=1"
    "SHELL:-s INITIAL_MEMORY=67108864"  # 64MB initial
    "SHELL:-s MODULARIZE=1"
    "SHELL:-s EXPORT_NAME='SPPlayer'"
    "SHELL:-s ENVIRONMENT='web'"
)
