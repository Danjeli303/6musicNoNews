#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
DEFAULT_SAMPLE_RECORDING="/Users/danielschembri/Desktop/iPlayer Recordings/Gilles_Peterson_-_A_tribute_to_Abdullah_Ibrahim_and_All_Winners_so_far_m002xmyp_original.m4a"
SAMPLE_RECORDING=${SAMPLE_RECORDING:-$DEFAULT_SAMPLE_RECORDING}
SLICE_START=${SLICE_START:-00:28:00}
SLICE_DURATION=${SLICE_DURATION:-00:09:00}
SLICE_DATE=${SLICE_DATE:-2026-06-20T15:28:00+01:00}

if [ "${SAMPLE_TEST_DIR:-}" ]; then
    WORK_DIR=$SAMPLE_TEST_DIR
    CLEAN_WORK_DIR=0
    mkdir -p "$WORK_DIR"
else
    WORK_DIR=$(mktemp -d "${TMPDIR:-/tmp}/skipper-sample-recording-test.XXXXXX")
    CLEAN_WORK_DIR=1
fi

cleanup() {
    if [ "$CLEAN_WORK_DIR" -eq 1 ] && [ "${KEEP_SAMPLE_TEST_DIR:-0}" != "1" ]; then
        rm -rf "$WORK_DIR"
    fi
}
trap cleanup EXIT HUP INT TERM

require_command() {
    if ! command -v "$1" >/dev/null 2>&1; then
        printf 'Error: required command not found: %s\n' "$1" >&2
        exit 1
    fi
}

assert_file_nonempty() {
    if [ ! -s "$1" ]; then
        printf 'Error: missing or empty file: %s\n' "$1" >&2
        exit 1
    fi
}

assert_contains() {
    file=$1
    pattern=$2
    label=$3

    if ! grep -E "$pattern" "$file" >/dev/null 2>&1; then
        printf 'Error: %s missing %s\n' "$file" "$label" >&2
        exit 1
    fi
}

assert_single_progress_line() {
    file=$1
    count=$(tr '\r' '\n' < "$file" | grep -E '^Processed: [0-9]+%' | wc -l | awk '{ print $1 }')
    newline_count=$(grep -E '^Processed: [0-9]+%' "$file" | wc -l | awk '{ print $1 }')

    if [ "$count" -lt 1 ]; then
        printf 'Error: %s missing carriage-return progress output\n' "$file" >&2
        exit 1
    fi

    if [ "$newline_count" -gt 1 ]; then
        printf 'Error: %s printed %s newline progress lines instead of rewriting one line\n' "$file" "$newline_count" >&2
        exit 1
    fi
}

ffprobe_value() {
    ffprobe \
      -hide_banner \
      -loglevel error \
      -show_entries "$1" \
      -of default=noprint_wrappers=1:nokey=1 \
      "$2" | head -n 1
}

video_stream_count() {
    ffprobe \
      -hide_banner \
      -loglevel error \
      -select_streams v \
      -show_entries stream=index \
      -of default=noprint_wrappers=1:nokey=1 \
      "$1" | wc -l | awk '{ print $1 }'
}

if [ "${RUN_LOCAL_SAMPLE_TEST:-0}" != "1" ] && [ "${SAMPLE_RECORDING:-}" = "$DEFAULT_SAMPLE_RECORDING" ]; then
    printf 'SKIP: local sample recording test is opt-in; set RUN_LOCAL_SAMPLE_TEST=1 or SAMPLE_RECORDING=/path/to/file.m4a\n' >&2
    exit 77
fi

if [ ! -f "$SAMPLE_RECORDING" ]; then
    printf 'Error: sample recording not found: %s\n' "$SAMPLE_RECORDING" >&2
    exit 1
fi

require_command awk
require_command ffmpeg
require_command ffprobe
require_command grep
require_command make

make -C "$SCRIPT_DIR" skipper >/dev/null

slice="$WORK_DIR/gilles_28_37.m4a"
output="$WORK_DIR/gilles_28_37_newsskip.m4a"
stdout="$WORK_DIR/gilles_28_37.stdout"
log="$WORK_DIR/gilles_28_37_newsskip.log"

ffmpeg \
  -hide_banner \
  -loglevel error \
  -y \
  -ss "$SLICE_START" \
  -t "$SLICE_DURATION" \
  -i "$SAMPLE_RECORDING" \
  -map 0 \
  -c copy \
  -metadata date="$SLICE_DATE" \
  "$slice"

assert_file_nonempty "$slice"

"$SCRIPT_DIR/skip_6music_news.sh" "$slice" >"$stdout"

assert_file_nonempty "$output"
assert_file_nonempty "$log"
assert_contains "$stdout" 'Processed: [0-9]+%' 'progress percentage'
assert_contains "$stdout" 'Processed: 100%' 'completion percentage'
assert_contains "$stdout" 'Skipper window: .*news_schedule\.ini' 'default skipper schedule'
assert_single_progress_line "$stdout"
assert_contains "$log" '(discarded|skip|skipped|talk|total input duration)' 'skipper processing evidence'

input_format=$(ffprobe_value format=format_name "$slice")
output_format=$(ffprobe_value format=format_name "$output")
input_codec=$(ffprobe_value stream=codec_name "$slice")
output_codec=$(ffprobe_value stream=codec_name "$output")
input_date=$(ffprobe_value format_tags=date "$slice")
output_date=$(ffprobe_value format_tags=date "$output")
input_title=$(ffprobe_value format_tags=title "$slice")
output_title=$(ffprobe_value format_tags=title "$output")
input_sample_rate=$(ffprobe_value stream=sample_rate "$slice")
output_sample_rate=$(ffprobe_value stream=sample_rate "$output")
input_channels=$(ffprobe_value stream=channels "$slice")
output_channels=$(ffprobe_value stream=channels "$output")
input_duration=$(ffprobe_value format=duration "$slice")
output_duration=$(ffprobe_value format=duration "$output")
input_video_count=$(video_stream_count "$slice")
output_video_count=$(video_stream_count "$output")

case "$input_format:$output_format" in
    *m4a*:*m4a*|*mov*mp4*m4a*:mov*mp4*m4a*)
        ;;
    *)
        printf 'Error: expected M4A input/output formats, got "%s" and "%s"\n' "$input_format" "$output_format" >&2
        exit 1
        ;;
esac

if [ "$input_date" != "$output_date" ] || [ "$input_title" != "$output_title" ]; then
    printf 'Error: output did not preserve sample metadata\n' >&2
    exit 1
fi

if [ "$input_sample_rate" != "$output_sample_rate" ] || [ "$input_channels" != "$output_channels" ]; then
    printf 'Error: output did not preserve audio format (%s/%s -> %s/%s)\n' \
      "$input_sample_rate" "$input_channels" "$output_sample_rate" "$output_channels" >&2
    exit 1
fi

if [ "$input_codec" != "$output_codec" ]; then
    printf 'Error: output did not preserve audio codec (%s -> %s)\n' "$input_codec" "$output_codec" >&2
    exit 1
fi

if [ "$input_video_count" != "$output_video_count" ]; then
    printf 'Error: output did not preserve artwork/video stream count (%s -> %s)\n' "$input_video_count" "$output_video_count" >&2
    exit 1
fi

if ! awk -v input="$input_duration" -v output="$output_duration" 'BEGIN { exit !(output > 0 && output < input) }'; then
    printf 'Error: expected news-skipped output duration %.3fs to be shorter than input %.3fs\n' \
      "$output_duration" "$input_duration" >&2
    exit 1
fi

printf 'OK: %s to %s sample conversion preserved M4A metadata/format and shortened the recording\n' "$SLICE_START" "$SLICE_DURATION"
