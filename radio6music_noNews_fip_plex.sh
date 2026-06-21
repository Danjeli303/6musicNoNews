#!/bin/sh
set -eu

BBC_URL="${BBC_URL:-http://as-hls-ww-live.akamaized.net/pool_81827798/live/ww/bbc_6music/bbc_6music.isml/bbc_6music-audio%3d320000.norewind.m3u8}"
FIP_URL="${FIP_URL:-https://stream.radiofrance.fr/fip/fip_hifi.m3u8?id=radiofrance}"
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
SILENCER="$SCRIPT_DIR/silencer"
SAMPLE_RATE=48000

OUT_DIR="${OUT_DIR:-$SCRIPT_DIR/plex_radio6music_noNews_fip}"
PLAYLIST="$OUT_DIR/radio6music_noNews_fip_plex.m3u8"
SEGMENT_PATTERN="$OUT_DIR/radio6music_noNews_fip_%05d.ts"
LOG="$OUT_DIR/radio6music_noNews_fip_plex.log"

FIP_VOLUME="${FIP_VOLUME:-0.85}"
DUCK_THRESHOLD="${DUCK_THRESHOLD:-0.002}"
DUCK_RATIO="${DUCK_RATIO:-20}"
FIP_FADE_OUT_MS="${FIP_FADE_OUT_MS:-700}"
FIP_FADE_IN_MS="${FIP_FADE_IN_MS:-1800}"
HLS_TIME="${HLS_TIME:-6}"
HLS_LIST_SIZE="${HLS_LIST_SIZE:-20}"

usage() {
    printf 'Usage: %s [--check]\n' "$0"
    printf 'Writes a rolling HLS audio stream to: %s\n' "$PLAYLIST"
    printf 'Set OUT_DIR=... to write the Plex-visible files elsewhere.\n'
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

get_stream_start_time() {
    playlist=$(curl -fsSL "$BBC_URL")
    start_time=$(printf '%s\n' "$playlist" | sed -n 's/^#EXT-X-PROGRAM-DATE-TIME://p' | head -n 1)

    if [ -z "$start_time" ]; then
        printf 'Error: BBC playlist did not include #EXT-X-PROGRAM-DATE-TIME\n' >&2
        exit 1
    fi

    printf '%s\n' "$start_time"
}

mix_with_fip_filter() {
    printf '[0:a]aformat=sample_fmts=fltp:channel_layouts=stereo,asplit=2[bbc][sc];'
    printf '[1:a]aresample=%s,aformat=sample_fmts=fltp:channel_layouts=stereo,volume=%s[fip];' "$SAMPLE_RATE" "$FIP_VOLUME"
    printf '[fip][sc]sidechaincompress=threshold=%s:ratio=%s:attack=%s:release=%s:makeup=1:link=maximum:detection=rms[fipduck];' "$DUCK_THRESHOLD" "$DUCK_RATIO" "$FIP_FADE_OUT_MS" "$FIP_FADE_IN_MS"
    printf '[bbc][fipduck]amix=inputs=2:duration=first:normalize=0:dropout_transition=0,alimiter=limit=0.95[out]'
}

prepare_output_dir() {
    mkdir -p "$OUT_DIR"
    rm -f "$PLAYLIST" "$OUT_DIR"/radio6music_noNews_fip_*.ts "$LOG"
}

run_pipeline() {
    duration_args=
    if [ "${1:-}" != "" ]; then
        duration_args="-t $1"
    fi

    # shellcheck disable=SC2086
    ffmpeg \
      -hide_banner \
      -loglevel warning \
      -reconnect 1 \
      -reconnect_streamed 1 \
      -reconnect_delay_max 2 \
      -i "$BBC_URL" \
      $duration_args \
      -f s16le -ar "$SAMPLE_RATE" -ac 2 pipe:1 2>>"$LOG" | \
    "$SILENCER" -t -x -v20 -s"$SAMPLE_RATE" -T "$START_TIME" -z "$LONDON_UTC_OFFSET" 2>>"$LOG" | \
    ffmpeg \
      -hide_banner \
      -loglevel warning \
      -f s16le -ar "$SAMPLE_RATE" -ac 2 -i pipe:0 \
      -reconnect 1 \
      -reconnect_streamed 1 \
      -reconnect_delay_max 2 \
      -i "$FIP_URL" \
      -filter_complex "$(mix_with_fip_filter)" \
      -map '[out]' \
      -c:a aac \
      -b:a 192k \
      -f hls \
      -hls_time "$HLS_TIME" \
      -hls_list_size "$HLS_LIST_SIZE" \
      -hls_flags delete_segments+program_date_time+omit_endlist \
      -hls_segment_filename "$SEGMENT_PATTERN" \
      "$PLAYLIST" 2>>"$LOG"
}

run_check() {
    prepare_output_dir
    run_pipeline 12

    if [ ! -s "$PLAYLIST" ]; then
        printf 'Error: HLS playlist was not created: %s\n' "$PLAYLIST" >&2
        exit 1
    fi

    if ! ls "$OUT_DIR"/radio6music_noNews_fip_*.ts >/dev/null 2>&1; then
        printf 'Error: HLS segments were not created in: %s\n' "$OUT_DIR" >&2
        exit 1
    fi

    printf 'OK: wrote HLS playlist: %s\n' "$PLAYLIST"
    printf 'OK: wrote HLS segments in: %s\n' "$OUT_DIR"
    printf 'OK: log: %s\n' "$LOG"
}

CHECK_ONLY=0
case "${1:-}" in
    "")
        ;;
    --check)
        CHECK_ONLY=1
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

require_command curl
require_command ffmpeg
require_command make
ensure_silencer

START_TIME=$(get_stream_start_time)
LONDON_UTC_OFFSET=$(get_london_utc_offset)

if [ "$CHECK_ONLY" -eq 1 ]; then
    run_check
    exit 0
fi

prepare_output_dir
printf 'Writing Plex HLS stream to: %s\n' "$PLAYLIST"
printf 'Log: %s\n' "$LOG"
printf 'Keep this script running while you listen.\n'
run_pipeline
