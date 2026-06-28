#!/bin/sh
set -eu

URL="${URL:-http://as-hls-ww-live.akamaized.net/pool_81827798/live/ww/bbc_6music/bbc_6music.isml/bbc_6music-audio%3d96000.norewind.m3u8}"
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
SILENCER="$SCRIPT_DIR/silencer"
SAMPLE_RATE=48000
SILENCER_WINDOW="${NEWS_SCHEDULE:-$SCRIPT_DIR/news_schedule.ini}"

usage() {
    printf 'Usage: %s [--check] [-w ranges-or-file]\n' "$0"
    printf 'Set URL=... to override the BBC 6 Music stream.\n'
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

get_london_utc_offset() {
    TZ=Europe/London date +%z | sed 's/^\([+-][0-9][0-9]\)\([0-9][0-9]\)$/\1:\2/'
}

# Raw PCM from ffmpeg has no place to carry HLS metadata, so grab the
# PROGRAM-DATE-TIME tag before starting the audio pipe and pass it to silencer.
get_stream_start_time() {
    playlist=$(curl -fsSL "$URL")
    start_time=$(printf '%s\n' "$playlist" | sed -n 's/^#EXT-X-PROGRAM-DATE-TIME://p' | head -n 1)

    if [ -z "$start_time" ]; then
        printf 'Error: stream playlist did not include #EXT-X-PROGRAM-DATE-TIME\n' >&2
        exit 1
    fi

    printf '%s\n' "$start_time"
}

# Smoke test the full input side without launching an endless ffplay session.
run_check() {
    tmp_pcm=$(mktemp "${TMPDIR:-/tmp}/sixmusic-silencer-check.XXXXXX")
    trap 'rm -f "$tmp_pcm"' EXIT HUP INT TERM

    ffmpeg \
      -hide_banner \
      -loglevel warning \
      -y \
      -reconnect 1 \
      -reconnect_streamed 1 \
      -reconnect_delay_max 2 \
      -i "$URL" \
      -t 1 \
      -f s16le -ar "$SAMPLE_RATE" -ac 2 "$tmp_pcm"

    if [ ! -s "$tmp_pcm" ]; then
        printf 'Error: ffmpeg did not produce any PCM audio during check\n' >&2
        exit 1
    fi

    "$SILENCER" -p -q -s"$SAMPLE_RATE" -T "$START_TIME" -z "$LONDON_UTC_OFFSET" -w "$SILENCER_WINDOW" < "$tmp_pcm" >/dev/null
    printf 'OK: stream timestamp: %s\n' "$START_TIME"
    printf 'OK: London UTC offset: %s\n' "$LONDON_UTC_OFFSET"
    printf 'OK: silencer window: %s\n' "$SILENCER_WINDOW"
    printf 'OK: ffmpeg decoded audio and silencer accepted it\n'
    printf 'OK: ffplay is installed for playback\n'
}

CHECK_ONLY=0
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
        *)
            usage >&2
            exit 1
            ;;
    esac
done

require_command curl
require_command ffmpeg
require_command ffplay
require_command make
ensure_silencer

START_TIME=$(get_stream_start_time)
LONDON_UTC_OFFSET=$(get_london_utc_offset)

# HLS timestamps are absolute instants; -z controls only how that instant is
# displayed for the London station clock.
if [ "$CHECK_ONLY" -eq 1 ]; then
    run_check
    exit 0
fi

ffmpeg \
  -hide_banner \
  -loglevel warning \
  -reconnect 1 \
  -reconnect_streamed 1 \
  -reconnect_delay_max 2 \
  -i "$URL" \
  -f s16le -ar "$SAMPLE_RATE" -ac 2 pipe:1 | \
"$SILENCER" -t -x -v20 -s"$SAMPLE_RATE" -T "$START_TIME" -z "$LONDON_UTC_OFFSET" -w "$SILENCER_WINDOW" | \
ffplay -hide_banner -loglevel warning -nodisp -f s16le -ar "$SAMPLE_RATE" -ch_layout stereo -
