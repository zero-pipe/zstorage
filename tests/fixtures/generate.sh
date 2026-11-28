#!/usr/bin/env bash
#
# Generate small deterministic media fixtures for ZCS tests.
# Requires ffmpeg (with libx264 + aac). H.265 / AV1 are optional.
#
#   ./generate.sh
#
set -euo pipefail

cd "$(dirname "$0")"

if ! command -v ffmpeg >/dev/null 2>&1; then
    echo "error: ffmpeg not found" >&2
    exit 1
fi

# Deterministic, metadata-free, short clips.
COMMON=(-hide_banner -loglevel error -y
        -f lavfi -i "testsrc2=size=320x240:rate=30"
        -f lavfi -i "sine=frequency=440:sample_rate=44100"
        -t 4 -map_metadata -1 -fflags +bitexact -flags:v +bitexact -flags:a +bitexact)

have_encoder() {
    # Read the full list (no grep -q) so ffmpeg does not die on SIGPIPE
    # under `set -o pipefail`.
    ffmpeg -hide_banner -encoders 2>&1 | grep " $1 " >/dev/null
}

echo "generating h264_aac.flv ..."
ffmpeg "${COMMON[@]}" -c:v libx264 -preset veryfast -g 30 -pix_fmt yuv420p \
    -c:a aac -b:a 64k -f flv h264_aac.flv

echo "generating h264_aac.ts ..."
ffmpeg "${COMMON[@]}" -c:v libx264 -preset veryfast -g 30 -pix_fmt yuv420p \
    -c:a aac -b:a 64k -f mpegts h264_aac.ts

echo "generating h264_aac.mp4 ..."
ffmpeg "${COMMON[@]}" -c:v libx264 -preset veryfast -g 30 -pix_fmt yuv420p \
    -c:a aac -b:a 64k -movflags +faststart h264_aac.mp4

echo "generating h264_aac.mkv ..."
ffmpeg "${COMMON[@]}" -c:v libx264 -preset veryfast -g 30 -pix_fmt yuv420p \
    -c:a aac -b:a 64k h264_aac.mkv

if have_encoder libx265; then
    echo "generating h265_aac.mp4 ..."
    ffmpeg "${COMMON[@]}" -c:v libx265 -preset ultrafast -x265-params log-level=none \
        -tag:v hvc1 -pix_fmt yuv420p -c:a aac -b:a 64k -movflags +faststart h265_aac.mp4
    echo "generating h265_aac.mkv ..."
    ffmpeg "${COMMON[@]}" -c:v libx265 -preset ultrafast -x265-params log-level=none \
        -pix_fmt yuv420p -c:a aac -b:a 64k h265_aac.mkv
else
    echo "skip h265 fixtures (libx265 not available)"
fi

if have_encoder libaom-av1 && have_encoder libopus; then
    echo "generating av1_opus.mp4 ..."
    ffmpeg "${COMMON[@]}" -c:v libaom-av1 -strict -2 -cpu-used 8 -crf 40 -g 30 \
        -c:a libopus -b:a 64k -movflags +faststart av1_opus.mp4
    echo "generating av1_opus.mkv ..."
    ffmpeg "${COMMON[@]}" -c:v libaom-av1 -strict -2 -cpu-used 8 -crf 40 -g 30 \
        -c:a libopus -b:a 64k av1_opus.mkv
else
    echo "skip av1 fixtures (libaom-av1 / libopus not available)"
fi

echo "done. generated files:"
ls -lh ./*.flv ./*.ts ./*.mp4 2>/dev/null || true
