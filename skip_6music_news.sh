#!/bin/sh
set -eu

DEFAULT_INPUT="/Users/danielschembri/Desktop/iPlayer Recordings/Gilles_Peterson_-_Zakia_Sewell_sits_in_Cameron_Winter_m002b79h_original.m4a"
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
SKIPPER="$SCRIPT_DIR/skipper"
SAMPLE_RATE=48000
BITRATE=192k
SKIPPER_WINDOW="${NEWS_SCHEDULE:-$SCRIPT_DIR/news_schedule.ini}"

usage() {
    printf 'Usage: %s [--check] [-w ranges-or-file] [input.ext] [output.ext]\n' "$0"
    printf 'Defaults to the Gilles Peterson iPlayer recording.\n'
    printf 'With one input file, writes input-name_newsskip.ext beside the input.\n'
    printf 'Default skipper schedule/window: %s\n' "$SKIPPER_WINDOW"
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
    dir=$(dirname -- "$1")
    base=$(basename -- "$1")
    base_no_ext=${base%.*}
    ext=${base##*.}
    printf '%s/%s_newsskip.%s\n' "$dir" "$base_no_ext" "$ext"
}

get_recorded_start_time() {
    start_time=$(ffprobe \
      -hide_banner \
      -loglevel warning \
      -show_entries format_tags=date \
      -of default=noprint_wrappers=1:nokey=1 \
      "$INPUT" | head -n 1)

    if [ -z "$start_time" ]; then
        start_time=$(ffprobe \
          -hide_banner \
          -loglevel warning \
          -select_streams a:0 \
          -show_entries stream_tags=date \
          -of default=noprint_wrappers=1:nokey=1 \
          "$INPUT" | head -n 1)
    fi

    if [ -z "$start_time" ]; then
        printf 'Error: input file has no date tag; pass a file with programme date metadata.\n' >&2
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

get_audio_stream_field() {
    ffprobe \
      -hide_banner \
      -loglevel warning \
      -select_streams a:0 \
      -show_entries "stream=$1" \
      -of default=noprint_wrappers=1:nokey=1 \
      "$INPUT" | head -n 1
}

get_input_duration_seconds() {
    ffprobe \
      -hide_banner \
      -loglevel warning \
      -show_entries format=duration \
      -of default=noprint_wrappers=1:nokey=1 \
      "$INPUT" | head -n 1
}

prepare_audio_format() {
    INPUT_CODEC=$(get_audio_stream_field codec_name)
    INPUT_SAMPLE_RATE=$(get_audio_stream_field sample_rate)
    INPUT_CHANNELS=$(get_audio_stream_field channels)
    INPUT_BITRATE=$(get_audio_stream_field bit_rate)
    INPUT_DURATION=$(get_input_duration_seconds)

    if [ -z "$INPUT_SAMPLE_RATE" ] || [ -z "$INPUT_CHANNELS" ]; then
        printf 'Error: could not read input audio stream format.\n' >&2
        exit 1
    fi

    if [ "$INPUT_CHANNELS" -ne 2 ]; then
        printf 'Error: input audio must be stereo; %s has %s channel(s).\n' "$INPUT" "$INPUT_CHANNELS" >&2
        exit 1
    fi

    case "$INPUT_BITRATE" in
        ''|N/A)
            INPUT_BITRATE=$BITRATE
            ;;
    esac

    case "$INPUT_DURATION" in
        ''|N/A)
            INPUT_DURATION=0
            ;;
    esac
}

lower_extension() {
    base=$(basename -- "$1")

    case "$base" in
        *.*)
            printf '%s\n' "${base##*.}" | tr '[:upper:]' '[:lower:]'
            ;;
        *)
            printf '\n'
            ;;
    esac
}

prepare_output_format() {
    INPUT_EXTENSION=$(lower_extension "$INPUT")
    OUTPUT_EXTENSION=$(lower_extension "$OUTPUT")

    if [ -z "$INPUT_EXTENSION" ]; then
        printf 'Error: input file must have an extension so the output format can match it: %s\n' "$INPUT" >&2
        exit 1
    fi

    if [ "$INPUT_EXTENSION" != "$OUTPUT_EXTENSION" ]; then
        printf 'Error: output extension must match input extension for strict format copying: .%s -> .%s\n' \
          "$INPUT_EXTENSION" "$OUTPUT_EXTENSION" >&2
        exit 1
    fi

    ARTWORK_MAP=
    VIDEO_CODEC_ARGS=
    MUXER_ARGS=
    FORMAT_ARGS=

    case "$INPUT_EXTENSION" in
        m4a|m4b|mp4|mp3|flac)
            ARTWORK_MAP="-map 1:v?"
            VIDEO_CODEC_ARGS="-c:v copy"
            ;;
    esac

    case "$INPUT_CODEC:$INPUT_EXTENSION" in
        aac:m4a|aac:m4b|aac:mp4)
            AUDIO_CODEC_ARGS="-c:a aac -b:a $INPUT_BITRATE -ar $INPUT_SAMPLE_RATE -ac $INPUT_CHANNELS"
            MUXER_ARGS="-movflags +faststart"
            case "$INPUT_EXTENSION" in
                mp4)
                    FORMAT_ARGS="-f mp4"
                    ;;
                *)
                    FORMAT_ARGS="-f ipod"
                    ;;
            esac
            ;;
        alac:m4a|alac:m4b|alac:mp4)
            AUDIO_CODEC_ARGS="-c:a alac -ar $INPUT_SAMPLE_RATE -ac $INPUT_CHANNELS"
            MUXER_ARGS="-movflags +faststart"
            case "$INPUT_EXTENSION" in
                mp4)
                    FORMAT_ARGS="-f mp4"
                    ;;
                *)
                    FORMAT_ARGS="-f ipod"
                    ;;
            esac
            ;;
        mp3:mp3)
            AUDIO_CODEC_ARGS="-c:a libmp3lame -b:a $INPUT_BITRATE -ar $INPUT_SAMPLE_RATE -ac $INPUT_CHANNELS"
            FORMAT_ARGS="-f mp3"
            ;;
        flac:flac)
            AUDIO_CODEC_ARGS="-c:a flac -ar $INPUT_SAMPLE_RATE -ac $INPUT_CHANNELS"
            FORMAT_ARGS="-f flac"
            ;;
        opus:opus|opus:ogg|opus:oga)
            AUDIO_CODEC_ARGS="-c:a libopus -b:a $INPUT_BITRATE -ar $INPUT_SAMPLE_RATE -ac $INPUT_CHANNELS"
            FORMAT_ARGS="-f ogg"
            ;;
        vorbis:ogg|vorbis:oga)
            AUDIO_CODEC_ARGS="-c:a libvorbis -b:a $INPUT_BITRATE -ar $INPUT_SAMPLE_RATE -ac $INPUT_CHANNELS"
            FORMAT_ARGS="-f ogg"
            ;;
        pcm_s16le:wav|pcm_s24le:wav|pcm_s32le:wav)
            AUDIO_CODEC_ARGS="-c:a $INPUT_CODEC -ar $INPUT_SAMPLE_RATE -ac $INPUT_CHANNELS"
            FORMAT_ARGS="-f wav"
            ;;
        *)
            printf 'Error: unsupported strict output format for codec/extension: %s/.%s\n' "$INPUT_CODEC" "$INPUT_EXTENSION" >&2
            printf 'Supported examples: AAC M4A/MP4, ALAC M4A/MP4, MP3, FLAC, Opus/Vorbis OGG, and WAV PCM.\n' >&2
            exit 1
            ;;
    esac
}

start_progress_monitor() {
    PROGRESS_DIR=$(mktemp -d "${TMPDIR:-/tmp}/recorded-skipper-progress.XXXXXX")
    PROGRESS_FIFO="$PROGRESS_DIR/progress.fifo"
    mkfifo "$PROGRESS_FIFO"

    awk -F '=' -v duration="$INPUT_DURATION" '
        BEGIN {
            last = -1
        }
        ($1 == "out_time_us" || $1 == "out_time_ms") && duration > 0 {
            pct = int((($2 / 1000000.0) / duration) * 100)
            if (pct > 100) {
                pct = 100
            }
            if (pct != last) {
                printf "\rProcessed: %d%%", pct
                fflush()
                last = pct
            }
        }
        $1 == "progress" && $2 == "end" {
            if (last < 100) {
                printf "\rProcessed: 100%%"
            }
            printf "\n"
            fflush()
        }
    ' < "$PROGRESS_FIFO" &
    PROGRESS_PID=$!
}

finish_progress_monitor() {
    if [ "${PROGRESS_PID:-}" ]; then
        wait "$PROGRESS_PID" || true
    fi
    PROGRESS_PID=
}

cleanup_progress_monitor() {
    if [ "${PROGRESS_PID:-}" ]; then
        kill "$PROGRESS_PID" 2>/dev/null || true
        wait "$PROGRESS_PID" 2>/dev/null || true
        PROGRESS_PID=
    fi

    if [ "${PROGRESS_DIR:-}" ]; then
        rm -rf "$PROGRESS_DIR"
        PROGRESS_DIR=
        PROGRESS_FIFO=
    fi
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
      -f s16le -ar "$INPUT_SAMPLE_RATE" -ac "$INPUT_CHANNELS" "$tmp_pcm"

    if [ ! -s "$tmp_pcm" ]; then
        printf 'Error: ffmpeg did not produce any PCM audio during check\n' >&2
        exit 1
    fi

    "$SKIPPER" -p -q -s"$INPUT_SAMPLE_RATE" -c"$INPUT_CHANNELS" -T "$START_TIME" -z "$UTC_OFFSET" -w "$SKIPPER_WINDOW" < "$tmp_pcm" >/dev/null
    printf 'OK: input: %s\n' "$INPUT"
    printf 'OK: audio: %s Hz, %s channels, %s bit/s target\n' "$INPUT_SAMPLE_RATE" "$INPUT_CHANNELS" "$INPUT_BITRATE"
    printf 'OK: skipper window: %s\n' "$SKIPPER_WINDOW"
    printf 'OK: programme start time: %s\n' "$START_TIME"
    printf 'OK: UTC offset: %s\n' "$UTC_OFFSET"
    printf 'OK: ffmpeg decoded audio and skipper accepted it\n'
}

CHECK_ONLY=0
INPUT_ARG=
OUTPUT_ARG=
while [ "$#" -gt 0 ]; do
    case "$1" in
        --check)
            CHECK_ONLY=1
            shift
            ;;
        -w|--window)
            if [ "$#" -lt 2 ]; then
                printf 'Error: %s requires minute ranges or a schedule file\n' "$1" >&2
                exit 1
            fi
            SKIPPER_WINDOW=$2
            shift 2
            ;;
        -w?*)
            SKIPPER_WINDOW=${1#-w}
            shift
            ;;
        --window=*)
            SKIPPER_WINDOW=${1#--window=}
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
ensure_skipper

START_TIME=$(get_recorded_start_time)
UTC_OFFSET=$(get_offset_from_timestamp "$START_TIME")
prepare_audio_format
prepare_output_format

if [ "$CHECK_ONLY" -eq 1 ]; then
    run_check
    exit 0
fi

printf 'Input: %s\n' "$INPUT"
printf 'Output: %s\n' "$OUTPUT"
printf 'Log: %s\n' "$LOG"
printf 'Audio: %s/%s, %s Hz, %s channels, %s bit/s target\n' "$INPUT_CODEC" "$INPUT_EXTENSION" "$INPUT_SAMPLE_RATE" "$INPUT_CHANNELS" "$INPUT_BITRATE"
printf 'Skipper window: %s\n' "$SKIPPER_WINDOW"
printf 'Programme start time: %s\n' "$START_TIME"
printf 'UTC offset: %s\n' "$UTC_OFFSET"

start_progress_monitor
trap cleanup_progress_monitor EXIT HUP INT TERM

ffmpeg \
  -hide_banner \
  -loglevel warning \
  -nostats \
  -progress "$PROGRESS_FIFO" \
  -i "$INPUT" \
  -vn \
  -f s16le -ar "$INPUT_SAMPLE_RATE" -ac "$INPUT_CHANNELS" pipe:1 2>"$LOG" | \
"$SKIPPER" -t -x -w "$SKIPPER_WINDOW" -s"$INPUT_SAMPLE_RATE" -c"$INPUT_CHANNELS" -T "$START_TIME" -z "$UTC_OFFSET" 2>>"$LOG" | \
ffmpeg \
  -hide_banner \
  -loglevel warning \
  -y \
  -f s16le -ar "$INPUT_SAMPLE_RATE" -ac "$INPUT_CHANNELS" -i pipe:0 \
  -i "$INPUT" \
  -map 0:a:0 \
  $ARTWORK_MAP \
  -map_metadata 1 \
  -map_metadata:s:a:0 1:s:a:0 \
  -map_chapters 1 \
  $AUDIO_CODEC_ARGS \
  $VIDEO_CODEC_ARGS \
  $MUXER_ARGS \
  $FORMAT_ARGS \
  "$OUTPUT" 2>>"$LOG"

finish_progress_monitor
cleanup_progress_monitor
trap - EXIT HUP INT TERM

printf 'Done: %s\n' "$OUTPUT"
