#!/bin/bash
# live-pipe.sh — the live pipeline for the Aquario STV-2000 (S905W).
#
# HLS input -> hardware decode -> overlay -> rawvideo pipe ->
# hardware encode (1080p, GOP 120) -> FLV/RTMP output.
#
# Two ffmpeg processes with a rawvideo pipe between them: the decoder's
# dmabuf frames must NOT reach the encoder zero-copy (see TESTING.md —
# the HCodec wedges on dmabuf input). The pipe forces a CPU copy.
#
# Usage: live-pipe.sh <output> <progress-file>
#   output: an .flv file, or an RTMP URL (rtmp://a.rtmp.youtube.com/live2/KEY)
#   INPUT_URL / OVERLAY_PNG are configurable below.
set -u

INPUT_URL="${INPUT_URL:-http://127.0.0.1:8000/test.m3u8}"
OVERLAY_PNG="${OVERLAY_PNG:-/tmp/ov.png}"
OUT="$1"
PROG="$2"

ffmpeg -v error -c:v h264_v4l2m2m -i "$INPUT_URL" -i "$OVERLAY_PNG" \
	-filter_complex "[0:v][1:v]overlay=10:10" \
	-f rawvideo -pix_fmt nv12 pipe:1 2>/tmp/dec-err.log | \
ffmpeg -y -v error -progress "$PROG" \
	-f rawvideo -pix_fmt nv12 -s 1920x1080 -r 30 -i pipe:0 \
	-c:v h264_v4l2m2m -pix_fmt nv12 -g 120 -f flv "$OUT" 2>/tmp/enc-err.log
