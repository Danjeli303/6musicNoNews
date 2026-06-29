#!/bin/sh
set -eu

DEFAULT_INPUT="/Users/danielschembri/Desktop/iPlayer Recordings/Gilles_Peterson_-_Zakia_Sewell_sits_in_Cameron_Winter_m002b79h_original.m4a"
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
SILENCER="$SCRIPT_DIR/silencer"
SAMPLE_RATE=48000
BITRATE=192k
AAC_CODER="${AAC_CODER:-fast}"
SILENCER_WINDOW="${NEWS_SCHEDULE:-$SCRIPT_DIR/news_schedule.ini}"

usage() {
    printf 'Usage: %s [--check] [--profile] [-w ranges-or-file] [input.m4a] [output.m4a]\n' "$0"
    printf 'Defaults to the Gilles Peterson iPlayer recording and writes beside this script.\n'
    printf 'Default silencer schedule/window: %s\n' "$SILENCER_WINDOW"
}

require_command() {
    if ! command -v "$1" >/dev/null 2>&1; then
        printf 'Error: required command not found: %s\n' "$1" >&2
        exit 1
    fi
}

ensure_silencer() {
    if [ ! -x "$SILENCER" ]; then
        make -C "$SCRIPT_DIR" silencer
    fi

    if [ ! -x "$SILENCER" ]; then
        printf 'Error: silencer executable was not found at %s\n' "$SILENCER" >&2
        exit 1
    fi
}

default_output_for_input() {
    base=$(basename -- "$1")
    base_no_ext=${base%.*}
    printf '%s/%s_silenced_talk.m4a\n' "$SCRIPT_DIR" "$base_no_ext"
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

run_profile_stage() {
    label=$1
    shift
    time_file=$(mktemp "${TMPDIR:-/tmp}/recorded-silencer-time.XXXXXX")

    if /usr/bin/time -p "$@" 2>"$time_file"; then
        status=0
    else
        status=$?
    fi

    awk '!/^(real|user|sys) /' "$time_file" >>"$LOG"
    real_seconds=$(awk '/^real / { value = $2 } END { print value }' "$time_file")
    rm -f "$time_file"

    PROFILE_TOTAL=$(awk -v total="$PROFILE_TOTAL" -v stage="$real_seconds" 'BEGIN { printf "%.2f", total + stage }')
    printf 'Profile: %-14s %6.2fs\n' "$label" "$real_seconds"
    return "$status"
}

cleanup_profile_files() {
    if [ "${PROFILE_DIR:-}" ]; then
        rm -rf "$PROFILE_DIR"
        PROFILE_DIR=
    fi
}

run_profile() {
    PROFILE_TOTAL=0
    PROFILE_DIR=$(mktemp -d "${TMPDIR:-/tmp}/recorded-silencer-profile.XXXXXX")
    profile_input_pcm="$PROFILE_DIR/input.pcm"
    profile_output_pcm="$PROFILE_DIR/output.pcm"
    trap cleanup_profile_files EXIT HUP INT TERM

    : >"$LOG"
    printf 'Profile mode: staged decode, silencer, encode\n'

    run_profile_stage "ffmpeg decode" \
      ffmpeg \
        -hide_banner \
        -loglevel warning \
        -y \
        -i "$INPUT" \
        -vn \
        -f s16le -ar "$SAMPLE_RATE" -ac 2 "$profile_input_pcm"

    run_profile_stage "silencer" \
      sh -c 'SILENCER_PROFILE=1 "$1" -t -x -s"$2" -T "$3" -z "$4" -w "$5" < "$6" > "$7"' \
        sh "$SILENCER" "$SAMPLE_RATE" "$START_TIME" "$UTC_OFFSET" "$SILENCER_WINDOW" "$profile_input_pcm" "$profile_output_pcm"

    run_profile_stage "ffmpeg encode" \
      ffmpeg \
        -hide_banner \
        -loglevel warning \
        -y \
        -f s16le -ar "$SAMPLE_RATE" -ac 2 -i "$profile_output_pcm" \
        -c:a aac -aac_coder "$AAC_CODER" -b:a "$BITRATE" \
        "$OUTPUT"

    cleanup_profile_files
    trap - EXIT HUP INT TERM
    printf 'Profile: %-14s %6.2fs\n' "total" "$PROFILE_TOTAL"
}

run_check() {
    tmp_pcm=$(mktemp "${TMPDIR:-/tmp}/recorded-silencer-check.XXXXXX")
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

    "$SILENCER" -p -q -s"$SAMPLE_RATE" -T "$START_TIME" -z "$UTC_OFFSET" -w "$SILENCER_WINDOW" < "$tmp_pcm" >/dev/null
    printf 'OK: input: %s\n' "$INPUT"
    printf 'OK: silencer window: %s\n' "$SILENCER_WINDOW"
    printf 'OK: programme start time: %s\n' "$START_TIME"
    printf 'OK: UTC offset: %s\n' "$UTC_OFFSET"
    printf 'OK: ffmpeg decoded audio and silencer accepted it\n'
}

CHECK_ONLY=0
PROFILE_ONLY=0
INPUT_ARG=
OUTPUT_ARG=
while [ "$#" -gt 0 ]; do
    case "$1" in
        --check)
            CHECK_ONLY=1
            shift
            ;;
        --profile)
            PROFILE_ONLY=1
            shift
            ;;
        -w|--window)
            if [ "$#" -lt 2 ]; then
                printf 'Error: %s requires minute ranges or a schedule file\n' "$1" >&2
                exit 1
            fi
            SILENCER_WINDOW=$2
            shift 2
            ;;
        -w?*)
            SILENCER_WINDOW=${1#-w}
            shift
            ;;
        --window=*)
            SILENCER_WINDOW=${1#--window=}
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        -*)
            printf 'Error: unknown option: %s\n' "$1" >&2
            usage >&2
            exit 1
            ;;
        *)
            if [ -z "$INPUT_ARG" ]; then
                INPUT_ARG=$1
            elif [ -z "$OUTPUT_ARG" ]; then
                OUTPUT_ARG=$1
            else
                usage >&2
                exit 1
            fi
            shift
            ;;
    esac
done

INPUT=${INPUT_ARG:-$DEFAULT_INPUT}
OUTPUT=${OUTPUT_ARG:-$(default_output_for_input "$INPUT")}
LOG="${OUTPUT%.*}.log"

if [ ! -f "$INPUT" ]; then
    printf 'Error: input file not found: %s\n' "$INPUT" >&2
    exit 1
fi

require_command ffmpeg
require_command ffprobe
require_command make
ensure_silencer

START_TIME=$(get_recorded_start_time)
UTC_OFFSET=$(get_offset_from_timestamp "$START_TIME")

if [ "$CHECK_ONLY" -eq 1 ]; then
    run_check
    exit 0
fi

printf 'Input: %s\n' "$INPUT"
printf 'Output: %s\n' "$OUTPUT"
printf 'Log: %s\n' "$LOG"
printf 'Silencer window: %s\n' "$SILENCER_WINDOW"
printf 'Programme start time: %s\n' "$START_TIME"
printf 'UTC offset: %s\n' "$UTC_OFFSET"

if [ "$PROFILE_ONLY" -eq 1 ]; then
    run_profile
    printf 'Done: %s\n' "$OUTPUT"
    exit 0
fi

ffmpeg \
  -hide_banner \
  -loglevel warning \
  -i "$INPUT" \
  -vn \
  -f s16le -ar "$SAMPLE_RATE" -ac 2 pipe:1 2>"$LOG" | \
"$SILENCER" -t -x -s"$SAMPLE_RATE" -T "$START_TIME" -z "$UTC_OFFSET" -w "$SILENCER_WINDOW" 2>>"$LOG" | \
ffmpeg \
  -hide_banner \
  -loglevel warning \
  -y \
  -f s16le -ar "$SAMPLE_RATE" -ac 2 -i pipe:0 \
  -c:a aac -aac_coder "$AAC_CODER" -b:a "$BITRATE" \
  "$OUTPUT" 2>>"$LOG"

printf 'Done: %s\n' "$OUTPUT"
