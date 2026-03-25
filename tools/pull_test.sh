#!/bin/bash
# Test pulling streams from SRS in various protocols.
#
# Usage: ./pull_test.sh [stream_name]

SRS_HOST="${SRS_HOST:-127.0.0.1}"
STREAM="${1:-live/test}"

echo "=== SRS Stream Pull URLs ==="
echo ""
echo "HTTP-FLV:  http://${SRS_HOST}:8080/${STREAM}.flv"
echo "HLS:       http://${SRS_HOST}:8080/${STREAM}.m3u8"
echo "RTMP:      rtmp://${SRS_HOST}/${STREAM}"
echo ""
echo "=== Play with sp_player ==="
echo "  sp_player http://${SRS_HOST}:8080/${STREAM}.flv     # HTTP-FLV (low latency)"
echo "  sp_player http://${SRS_HOST}:8080/${STREAM}.m3u8    # HLS"
echo "  sp_player rtmp://${SRS_HOST}/${STREAM}              # RTMP"
echo ""
echo "=== Checking SRS API ==="
curl -s "http://${SRS_HOST}:1985/api/v1/streams" 2>/dev/null | python3 -m json.tool 2>/dev/null || echo "(SRS not running or no streams)"
