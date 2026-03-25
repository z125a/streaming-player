cmake_minimum_required(VERSION 3.18)
project(sp_android)

set(CMAKE_CXX_STANDARD 17)

# FFmpeg prebuilt libraries (user must provide these)
# Set FFMPEG_DIR to the path containing include/ and lib/<ABI>/
set(FFMPEG_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../third_party/ffmpeg-android"
    CACHE PATH "Path to FFmpeg Android prebuilt")

# Core includes
set(CORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../core")

add_library(sp_player SHARED native_player.cpp)

target_include_directories(sp_player PRIVATE
    ${CORE_DIR}
    ${FFMPEG_DIR}/include
)

# Link FFmpeg libs
foreach(LIB avformat avcodec avutil swscale swresample)
    add_library(${LIB} SHARED IMPORTED)
    set_target_properties(${LIB} PROPERTIES
        IMPORTED_LOCATION "${FFMPEG_DIR}/lib/${ANDROID_ABI}/lib${LIB}.so")
    target_link_libraries(sp_player PRIVATE ${LIB})
endforeach()

# Android system libs
target_link_libraries(sp_player PRIVATE
    android
    log
    mediandk    # For MediaCodec NDK API
    OpenSLES    # For audio output
)
