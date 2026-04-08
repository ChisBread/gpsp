#!/bin/bash
# Convert raw frames + PCM audio into an mp4 video.
#
# Usage: ./make_video.sh <frames.bin> <audio.pcm> [output.mp4]
#
# frames.bin: 240×160 u16 (RGB565LE) pixels, one frame per 76800 bytes
# audio.pcm:  s16le stereo @ 65536 Hz

set -euo pipefail

FRAMES="${1:?Usage: $0 <frames.bin> <audio.pcm> [output.mp4]}"
AUDIO="${2:?Usage: $0 <frames.bin> <audio.pcm> [output.mp4]}"
OUTPUT="${3:-output.mp4}"

FRAME_BYTES=$((240 * 160 * 2))
FILE_SIZE=$(stat -c%s "$FRAMES")
NUM_FRAMES=$((FILE_SIZE / FRAME_BYTES))

echo "Frames: $NUM_FRAMES (from $FRAMES)"
echo "Audio:  $AUDIO"
echo "Output: $OUTPUT"

ffmpeg -y \
  -f rawvideo -pixel_format rgb565le -video_size 240x160 -framerate 59.7275 \
  -i "$FRAMES" \
  -f s16le -ar 65536 -ac 2 \
  -i "$AUDIO" \
  -vf "scale=720:480:flags=neighbor" \
  -c:v libx264 -preset fast -crf 18 -pix_fmt yuv420p \
  -c:a libmp3lame -b:a 128k \
  -shortest \
  "$OUTPUT"

echo "Done: $OUTPUT"
