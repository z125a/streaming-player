#!/bin/bash
# Start a lightweight HLS live streaming server using FFmpeg + Python HTTP.
# No Docker needed!
#
# Usage:
#   ./start_hls_server.sh [input_file]
#
# This will:
# 1. Create HLS segments from the input (or test pattern)
# 2. Serve them via Python HTTP server on port 8080
#
# Pull URL: http://127.0.0.1:8080/live/stream.m3u8

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
HLS_DIR="/tmp/sp_hls_server"
INPUT="${1:-testsrc}"
PORT="${PORT:-8080}"

# Cleanup
rm -rf "$HLS_DIR"
mkdir -p "$HLS_DIR/live"

echo "=== Streaming Player - HLS Server ==="
echo "HLS dir: $HLS_DIR"
echo "Pull URL: http://127.0.0.1:${PORT}/live/stream.m3u8"
echo ""

# Start Python HTTP server in background
cd "$HLS_DIR"
python3 -m http.server "$PORT" --bind 0.0.0.0 &
HTTP_PID=$!
echo "HTTP server started (PID: $HTTP_PID)"

cleanup() {
    echo ""
    echo "Stopping..."
    kill $HTTP_PID 2>/dev/null || true
    kill $FFMPEG_PID 2>/dev/null || true
    rm -rf "$HLS_DIR"
    echo "Done."
}
trap cleanup EXIT INT TERM

# Start FFmpeg to generate HLS segments
if [ "$INPUT" = "testsrc" ]; then
    echo "Source: Test pattern (continuous, Ctrl+C to stop)"
    ffmpeg -re \
        -f lavfi -i "testsrc2=size=640x480:rate=30" \
        -f lavfi -i "sine=frequency=440:sample_rate=44100" \
        -c:v libx264 -preset ultrafast -tune zerolatency \
        -b:v 800k -g 30 -pix_fmt yuv420p \
        -c:a aac -b:a 96k -ar 44100 \
        -f hls \
        -hls_time 2 \
        -hls_list_size 5 \
        -hls_flags delete_segments+append_list \
        -hls_segment_filename "$HLS_DIR/live/stream_%03d.ts" \
        "$HLS_DIR/live/stream.m3u8" &
    FFMPEG_PID=$!
else
    echo "Source: $INPUT (loop mode)"
    ffmpeg -re -stream_loop -1 -i "$INPUT" \
        -c:v libx264 -preset ultrafast -tune zerolatency \
        -b:v 800k -g 30 -pix_fmt yuv420p \
        -c:a aac -b:a 96k -ar 44100 \
        -f hls \
        -hls_time 2 \
        -hls_list_size 5 \
        -hls_flags delete_segments+append_list \
        -hls_segment_filename "$HLS_DIR/live/stream_%03d.ts" \
        "$HLS_DIR/live/stream.m3u8" &
    FFMPEG_PID=$!
fi

echo "FFmpeg started (PID: $FFMPEG_PID)"
echo ""
echo "=== Server running ==="
echo "Play: sp_player http://127.0.0.1:${PORT}/live/stream.m3u8"
echo "Press Ctrl+C to stop"

wait $FFMPEG_PID
