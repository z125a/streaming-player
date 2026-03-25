#!/bin/bash
# Push a test stream to SRS server via RTMP.
#
# Usage:
#   ./push_stream.sh [input_file] [stream_name]
#
# Examples:
#   ./push_stream.sh                           # generate test pattern → live/test
#   ./push_stream.sh video.mp4                 # push file → live/test
#   ./push_stream.sh video.mp4 live/mystream   # push file → live/mystream

set -e

SRS_HOST="${SRS_HOST:-127.0.0.1}"
INPUT="${1:-testsrc}"
STREAM="${2:-live/test}"
RTMP_URL="rtmp://${SRS_HOST}/${STREAM}"

echo "=== Streaming Player - Push Stream ==="
echo "Target: ${RTMP_URL}"

if [ "$INPUT" = "testsrc" ]; then
    echo "Source: Test pattern (640x480, 30fps, 440Hz tone)"
    echo "Press Ctrl+C to stop"
    ffmpeg -re \
        -f lavfi -i "testsrc2=size=640x480:rate=30" \
        -f lavfi -i "sine=frequency=440:sample_rate=44100" \
        -c:v libx264 -preset ultrafast -tune zerolatency \
        -b:v 1000k -maxrate 1000k -bufsize 2000k \
        -pix_fmt yuv420p -g 60 \
        -c:a aac -b:a 128k -ar 44100 \
        -f flv "${RTMP_URL}"
else
    if [ ! -f "$INPUT" ]; then
        echo "Error: File not found: $INPUT"
        exit 1
    fi
    echo "Source: ${INPUT}"
    echo "Press Ctrl+C to stop"
    ffmpeg -re -i "${INPUT}" \
        -c:v libx264 -preset ultrafast -tune zerolatency \
        -b:v 1000k -maxrate 1000k -bufsize 2000k \
        -pix_fmt yuv420p -g 60 \
        -c:a aac -b:a 128k -ar 44100 \
        -f flv "${RTMP_URL}"
fi
