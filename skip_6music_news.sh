#!/bin/sh
set -eu

DEFAULT_INPUT="/Users/danielschembri/Desktop/iPlayer Recordings/Gilles_Peterson_-_Zakia_Sewell_sits_in_Cameron_Winter_m002b79h_original.m4a"
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
SKIPPER="$SCRIPT_DIR/skipper"
SAMPLE_RATE=48000
BITRATE=192k

usage() {
    printf 'Usage: %s [--check] [input.m4a] [output.m4a]\n' "$0"
    printf 'Defaults to the Gilles Peterson iPlayer recording and writes beside this script.\n'
}

require_command() {
    if ! command -v "$1" >/dev/null 2>&1; then
        printf 'Error: required command not found: %s\n' "$1" >&2
        exit 1
    fi
}

ensure_skipper() {
    if [ ! -x "$SKIPPER" ]; then
        make -C "$SCRIPT_DIR" skipper
    fi

    if [ ! -x "$SKIPPER" ]; then
        printf 'Error: skipper executable was not found at %s\n' "$SKIPPER" >&2
        exit 1
    fi
}

default_output_for_input() {
    base=$(basename -- "$1")
    base_no_ext=${base%.*}
    printf '%s/%s_skipped_news.m4a\n' "$SCRIPT_DIR" "$base_no_ext"
}

get_recorded_start_time() {
    start_time=$(ffprobe \
      -hide_banner \
      -loglevel warning \
      -show_entries format_tags=date \
      -of default=noprint_wrappers=1:nokey=1 \
      "$INPUT" | head -n 1)

    if [ -z "$start_time" ]; then
        printf 'Error: input file has no format date tag; pass a file with programme date metadata.\n' >&2
        exit 1
    fi

    printf '%s\n' "$start_time"
}

get_offset_from_timestamp() {
    case "$1" in
        *[+-][0-9][0-9]:[0-9][0-9])
            printf '%s\n' "$1" | sed 's/^.*\([+-][0-9][0-9]:[0-9][0-9]\)$/\1/'
            ;;
        *Z|*z)
            printf '+00:00\n'
            ;;
        *)
            printf 'Error: programme date does not include a UTC offset: %s\n' "$1" >&2
            exit 1
            ;;
    esac
}

run_check() {
    tmp_pcm=$(mktemp "${TMPDIR:-/tmp}/recorded-skipper-check.XXXXXX")
    trap 'rm -f "$tmp_pcm"' EXIT HUP INT TERM

    ffmpeg \
      -hide_banner \
      -loglevel warning \
      -y \
      -i "$INPUT" \
      -vn \
      -t 1 \
      -f s16le -ar "$SAMPLE_RATE" -ac 2 "$tmp_pcm"

    if [ ! -s "$tmp_pcm" ]; then
        printf 'Error: ffmpeg did not produce any PCM audio during check\n' >&2
        exit 1
    fi

    "$SKIPPER" -p -q -s"$SAMPLE_RATE" -T "$START_TIME" -z "$UTC_OFFSET" < "$tmp_pcm" >/dev/null
    printf 'OK: input: %s\n' "$INPUT"
    printf 'OK: programme start time: %s\n' "$START_TIME"
    printf 'OK: UTC offset: %s\n' "$UTC_OFFSET"
    printf 'OK: ffmpeg decoded audio and skipper accepted it\n'
}

CHECK_ONLY=0
case "${1:-}" in
    --check)
        CHECK_ONLY=1
        shift
        ;;
    -h|--help)
        usage
        exit 0
        ;;
esac

if [ "$#" -gt 2 ]; then
    usage >&2
    exit 1
fi

INPUT=${1:-$DEFAULT_INPUT}
OUTPUT=${2:-$(default_output_for_input "$INPUT")}
LOG="${OUTPUT%.*}.log"

if [ ! -f "$INPUT" ]; then
    printf 'Error: input file not found: %s\n' "$INPUT" >&2
    exit 1
fi

require_command ffmpeg
require_command ffprobe
require_command make
ensure_skipper

START_TIME=$(get_recorded_start_time)
UTC_OFFSET=$(get_offset_from_timestamp "$START_TIME")

if [ "$CHECK_ONLY" -eq 1 ]; then
    run_check
    exit 0
fi

printf 'Input: %s\n' "$INPUT"
printf 'Output: %s\n' "$OUTPUT"
printf 'Log: %s\n' "$LOG"
printf 'Programme start time: %s\n' "$START_TIME"
printf 'UTC offset: %s\n' "$UTC_OFFSET"

ffmpeg \
  -hide_banner \
  -loglevel warning \
  -i "$INPUT" \
  -vn \
  -f s16le -ar "$SAMPLE_RATE" -ac 2 pipe:1 2>"$LOG" | \
"$SKIPPER" -t -x -s"$SAMPLE_RATE" -T "$START_TIME" -z "$UTC_OFFSET" 2>>"$LOG" | \
ffmpeg \
  -hide_banner \
  -loglevel warning \
  -y \
  -f s16le -ar "$SAMPLE_RATE" -ac 2 -i pipe:0 \
  -c:a aac -b:a "$BITRATE" \
  "$OUTPUT" 2>>"$LOG"

printf 'Done: %s\n' "$OUTPUT"
