#!/bin/sh
set -eu

BBC_URL="${BBC_URL:-http://as-hls-ww-live.akamaized.net/pool_81827798/live/ww/bbc_6music/bbc_6music.isml/bbc_6music-audio%3d320000.norewind.m3u8}"
FIP_URL="${FIP_URL:-https://stream.radiofrance.fr/fip/fip_hifi.m3u8?id=radiofrance}"
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
NEWS_IDENTIFIER="$SCRIPT_DIR/news_identifier"
NEWS_MIXER_CONTROL="$SCRIPT_DIR/news_mixer_control"
SAMPLE_RATE=48000
NEWS_WINDOW="${NEWS_SCHEDULE:-$SCRIPT_DIR/news_schedule.ini}"
IDENTIFIER_DELAY_SECONDS=22
MIXER_CONTROL_ENDPOINT="${MIXER_CONTROL_ENDPOINT:-tcp://127.0.0.1:5555}"

OUT_DIR="${OUT_DIR:-$SCRIPT_DIR/hls_radio6music_noNews}"
PLAYLIST="$OUT_DIR/radio6music_noNews.m3u8"
SEGMENT_PATTERN="$OUT_DIR/radio6music_noNews_%05d.ts"
LOG="$OUT_DIR/radio6music_noNews_hls.log"
NEWS_STATUS_FILE="${NEWS_STATUS_FILE:-$OUT_DIR/news-status.json}"

FIP_VOLUME="${FIP_VOLUME:-0.85}"
BBC_FADE_OUT_MS="${BBC_FADE_OUT_MS:-1200}"
BBC_FADE_IN_MS="${BBC_FADE_IN_MS:-1800}"
HLS_AUDIO_BITRATE="${HLS_AUDIO_BITRATE:-128k}"
HLS_AAC_CODER="${HLS_AAC_CODER:-fast}"
HLS_TIME="${HLS_TIME:-6}"
HLS_LIST_SIZE="${HLS_LIST_SIZE:-20}"
HLS_RESTART_DELAY_SECONDS="${HLS_RESTART_DELAY_SECONDS:-1}"
HLS_CLEAN_START="${HLS_CLEAN_START:-0}"

usage() {
    printf 'Usage: %s [--check]\n' "$0"
    printf 'Writes a rolling HLS audio stream to: %s\n' "$PLAYLIST"
    printf 'Set OUT_DIR=... to write the HLS files elsewhere.\n'
    printf 'Default news schedule/window: %s\n' "$NEWS_WINDOW"
}

require_command() {
    if ! command -v "$1" >/dev/null 2>&1; then
        printf 'Error: required command not found: %s\n' "$1" >&2
        exit 1
    fi
}

ensure_live_programs() {
    if command -v make >/dev/null 2>&1; then
        if ! make -C "$SCRIPT_DIR" -q news_identifier news_mixer_control >/dev/null 2>&1; then
            make -C "$SCRIPT_DIR" news_identifier news_mixer_control
        fi
    elif [ ! -x "$NEWS_IDENTIFIER" ] || [ ! -x "$NEWS_MIXER_CONTROL" ]; then
        printf 'Error: live news tools must be built, but make was not found.\n' >&2
        exit 1
    fi

    if [ ! -x "$NEWS_IDENTIFIER" ] || [ ! -x "$NEWS_MIXER_CONTROL" ]; then
        printf 'Error: news identifier or mixer controller executable was not found.\n' >&2
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
    delay_ms=$((IDENTIFIER_DELAY_SECONDS * 1000))
    # FFmpeg parses the filter graph and the azmq option separately, so each
    # colon needs two backslashes at the filter-graph layer.
    zmq_address=$(printf '%s' "$MIXER_CONTROL_ENDPOINT" | sed 's/:/\\\\:/g')
    printf '[0:a]aformat=sample_fmts=fltp:channel_layouts=stereo,adelay=%s:all=1,volume@bbc=1[bbc];' "$delay_ms"
    printf '[1:a]aresample=%s,aformat=sample_fmts=fltp:channel_layouts=stereo,volume@fip=0[fip];' "$SAMPLE_RATE"
    printf '[bbc][fip]amix=inputs=2:duration=first:normalize=0:dropout_transition=0,'
    printf 'alimiter=limit=0.95,azmq=bind_address=%s,aresample=async=1000:first_pts=0[out]' "$zmq_address"
}

start_news_workers() {
    NEWS_WORK_DIR=$(mktemp -d "${TMPDIR:-/tmp}/news-identifier.XXXXXX")
    NEWS_AUDIO_PIPE="$NEWS_WORK_DIR/audio.fifo"
    NEWS_EVENT_PIPE="$NEWS_WORK_DIR/events.fifo"
    mkfifo "$NEWS_AUDIO_PIPE" "$NEWS_EVENT_PIPE"

    "$NEWS_MIXER_CONTROL" "$NEWS_STATUS_FILE" "$MIXER_CONTROL_ENDPOINT" \
      "$FIP_VOLUME" "$BBC_FADE_OUT_MS" "$BBC_FADE_IN_MS" \
      <"$NEWS_EVENT_PIPE" >>"$LOG" 2>&1 &
    NEWS_MIXER_CONTROL_PID=$!

    "$NEWS_IDENTIFIER" -e -t -x -v20 -s"$SAMPLE_RATE" -T "$START_TIME" \
      -z "$LONDON_UTC_OFFSET" -w "$NEWS_WINDOW" \
      <"$NEWS_AUDIO_PIPE" >/dev/null 2>"$NEWS_EVENT_PIPE" &
    NEWS_IDENTIFIER_PID=$!
}

stop_news_workers() {
    wait "$NEWS_IDENTIFIER_PID" || true
    wait "$NEWS_MIXER_CONTROL_PID" || true
    rm -f "$NEWS_AUDIO_PIPE" "$NEWS_EVENT_PIPE"
    rmdir "$NEWS_WORK_DIR"
}

ensure_output_dir() {
    mkdir -p "$OUT_DIR"
}

clean_output_dir() {
    ensure_output_dir
    rm -f "$PLAYLIST" "$OUT_DIR"/radio6music_noNews_*.ts "$LOG" "$NEWS_STATUS_FILE"
}

ffmpeg_live_input_args() {
    printf '%s\n' \
      -reconnect 1 \
      -reconnect_streamed 1 \
      -reconnect_on_network_error 1 \
      -reconnect_on_http_error 4xx,5xx \
      -reconnect_delay_max 10 \
      -rw_timeout 15000000
}

run_pipeline() {
    duration_args=
    if [ "${1:-}" != "" ]; then
        duration_args="-t $1"
    fi

    start_news_workers

    # live_start_index 0 matches FFmpeg's first decoded segment to the first
    # PROGRAM-DATE-TIME captured from the same media playlist.
    # shellcheck disable=SC2086
    ffmpeg \
      -hide_banner \
      -loglevel warning \
      $(ffmpeg_live_input_args) \
      -live_start_index 0 \
      -i "$BBC_URL" \
      $duration_args \
      -f s16le -ar "$SAMPLE_RATE" -ac 2 pipe:1 2>>"$LOG" | \
    tee "$NEWS_AUDIO_PIPE" | \
    ffmpeg \
      -hide_banner \
      -loglevel warning \
      -thread_queue_size 4096 \
      -f s16le -ar "$SAMPLE_RATE" -ac 2 -channel_layout stereo -i pipe:0 \
      $(ffmpeg_live_input_args) \
      -thread_queue_size 4096 \
      -i "$FIP_URL" \
      -filter_complex "$(mix_with_fip_filter)" \
      -map '[out]' \
      -c:a aac \
      -aac_coder "$HLS_AAC_CODER" \
      -b:a "$HLS_AUDIO_BITRATE" \
      -f hls \
      -hls_time "$HLS_TIME" \
      -hls_list_size "$HLS_LIST_SIZE" \
      -hls_start_number_source epoch \
      -hls_flags append_list+delete_segments+program_date_time+omit_endlist+temp_file \
      -hls_segment_filename "$SEGMENT_PATTERN" \
      "$PLAYLIST" 2>>"$LOG"
    pipeline_status=$?
    stop_news_workers
    return "$pipeline_status"
}

run_check() {
    clean_output_dir
    run_pipeline 12

    if [ ! -s "$PLAYLIST" ]; then
        printf 'Error: HLS playlist was not created: %s\n' "$PLAYLIST" >&2
        exit 1
    fi

    segment_found=0
    for segment in "$OUT_DIR"/radio6music_noNews_*.ts; do
        if [ -s "$segment" ]; then
            segment_found=1
            break
        fi
    done
    if [ "$segment_found" -ne 1 ] || ! grep -q '^#EXTINF:' "$PLAYLIST"; then
        printf 'Error: no playable HLS segment was created in: %s\n' "$OUT_DIR" >&2
        exit 1
    fi

    printf 'OK: wrote HLS playlist: %s\n' "$PLAYLIST"
    printf 'OK: wrote HLS segments in: %s\n' "$OUT_DIR"
    printf 'OK: log: %s\n' "$LOG"
}

run_pipeline_forever() {
    while :; do
        START_TIME=$(get_stream_start_time)
        LONDON_UTC_OFFSET=$(get_london_utc_offset)

        printf 'BBC stream timestamp: %s\n' "$START_TIME" >&2
        printf 'London UTC offset: %s\n' "$LONDON_UTC_OFFSET" >&2
        printf 'News window: %s\n' "$NEWS_WINDOW" >&2
        printf 'HLS audio bitrate: %s\n' "$HLS_AUDIO_BITRATE" >&2
        printf 'HLS segment length: %s seconds\n' "$HLS_TIME" >&2
        printf 'HLS list size: %s segments\n' "$HLS_LIST_SIZE" >&2
        ensure_output_dir
        if run_pipeline; then
            printf 'HLS pipeline ended; restarting in %s seconds.\n' "$HLS_RESTART_DELAY_SECONDS" >&2
        else
            printf 'HLS pipeline failed; restarting in %s seconds.\n' "$HLS_RESTART_DELAY_SECONDS" >&2
        fi

        sleep "$HLS_RESTART_DELAY_SECONDS"
    done
}

CHECK_ONLY=0
while [ "$#" -gt 0 ]; do
    case "$1" in
        --check)
            CHECK_ONLY=1
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
ensure_live_programs

if [ "$CHECK_ONLY" -eq 1 ]; then
    START_TIME=$(get_stream_start_time)
    LONDON_UTC_OFFSET=$(get_london_utc_offset)
    run_check
    exit 0
fi

printf 'Writing HLS stream to: %s\n' "$PLAYLIST"
printf 'Log: %s\n' "$LOG"
printf 'News window: %s\n' "$NEWS_WINDOW"
printf 'Keep this script running while you listen.\n'
case "$HLS_CLEAN_START" in
    1|true|TRUE|yes|YES)
        clean_output_dir
        ;;
esac
run_pipeline_forever
