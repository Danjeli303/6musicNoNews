#!/bin/sh
set -eu

BBC_URL="${BBC_URL:-http://as-hls-ww-live.akamaized.net/pool_81827798/live/ww/bbc_6music/bbc_6music.isml/bbc_6music-audio%3d320000.norewind.m3u8}"
FIP_URL="${FIP_URL:-https://stream.radiofrance.fr/fip/fip_hifi.m3u8?id=radiofrance}"
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
SILENCER="$SCRIPT_DIR/silencer"
SAMPLE_RATE=48000
SILENCER_WINDOW="${NEWS_SCHEDULE:-$SCRIPT_DIR/news_schedule.ini}"

# FIP is ducked by the silenced 6 Music signal. Release controls FIP fade-in
# when 6 Music becomes silent; attack controls FIP fade-out when 6 Music returns.
FIP_VOLUME="${FIP_VOLUME:-0.85}"
DUCK_THRESHOLD="${DUCK_THRESHOLD:-0.002}"
DUCK_RATIO="${DUCK_RATIO:-20}"
FIP_FADE_OUT_MS="${FIP_FADE_OUT_MS:-700}"
FIP_FADE_IN_MS="${FIP_FADE_IN_MS:-1800}"
AWS_AUDIO_BITRATE="${AWS_AUDIO_BITRATE:-128k}"
AWS_RESTART_DELAY_SECONDS="${AWS_RESTART_DELAY_SECONDS:-5}"
AWS_ICECAST_URL="${AWS_ICECAST_URL:-}"
AWS_ICECAST_HOST="${AWS_ICECAST_HOST:-icecast}"
AWS_ICECAST_PORT="${AWS_ICECAST_PORT:-8000}"
AWS_ICECAST_USER="${AWS_ICECAST_USER:-source}"
AWS_ICECAST_MOUNT="${AWS_ICECAST_MOUNT:-the-radio.mp3}"

usage() {
    printf 'Usage: %s [--check|-AWS] [-w ranges-or-file]\n' "$0"
    printf 'Set BBC_URL=... or FIP_URL=... to override either stream.\n'
    printf 'Default silencer schedule/window: %s\n' "$SILENCER_WINDOW"
    printf 'With -AWS, set ICECAST_SOURCE_PASSWORD=... or AWS_ICECAST_URL=icecast://source:password@host:8000/mount.mp3.\n'
}

require_command() {
    if ! command -v "$1" >/dev/null 2>&1; then
        printf 'Error: required command not found: %s\n' "$1" >&2
        exit 1
    fi
}

ensure_silencer() {
    if [ ! -x "$SILENCER" ]; then
        require_command make
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
    printf '[bbc][fipduck]amix=inputs=2:duration=first:normalize=0:dropout_transition=0,alimiter=limit=0.95,aresample=async=1000:first_pts=0[out]'
}

build_aws_icecast_url() {
    mount="${AWS_ICECAST_MOUNT#/}"

    if [ -n "$AWS_ICECAST_URL" ]; then
        printf '%s\n' "$AWS_ICECAST_URL"
        return 0
    fi

    if [ -z "${ICECAST_SOURCE_PASSWORD:-}" ]; then
        printf 'Error: -AWS requires ICECAST_SOURCE_PASSWORD or AWS_ICECAST_URL.\n' >&2
        printf 'Example: ICECAST_SOURCE_PASSWORD=change-me %s -AWS\n' "$0" >&2
        exit 1
    fi

    if [ -z "$mount" ]; then
        printf 'Error: AWS_ICECAST_MOUNT must not be empty.\n' >&2
        exit 1
    fi

    printf 'icecast://%s:%s@%s:%s/%s\n' "$AWS_ICECAST_USER" "$ICECAST_SOURCE_PASSWORD" "$AWS_ICECAST_HOST" "$AWS_ICECAST_PORT" "$mount"
}

redact_url_password() {
    printf '%s\n' "$1" | sed 's#//\([^:/@]*\):[^@]*@#//\1:***@#'
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

run_check() {
    check_seconds=3

    ffmpeg \
      -hide_banner \
      -loglevel warning \
      $(ffmpeg_live_input_args) \
      -i "$BBC_URL" \
      -t "$check_seconds" \
      -f s16le -ar "$SAMPLE_RATE" -ac 2 pipe:1 | \
    "$SILENCER" -t -x -q -s"$SAMPLE_RATE" -T "$START_TIME" -z "$LONDON_UTC_OFFSET" -w "$SILENCER_WINDOW" | \
    ffmpeg \
      -hide_banner \
      -loglevel warning \
      -re \
      -f s16le -ar "$SAMPLE_RATE" -ac 2 -i pipe:0 \
      $(ffmpeg_live_input_args) \
      -i "$FIP_URL" \
      -filter_complex "$(mix_with_fip_filter)" \
      -map '[out]' \
      -t "$check_seconds" \
      -f null -

    printf 'OK: BBC stream timestamp: %s\n' "$START_TIME"
    printf 'OK: London UTC offset: %s\n' "$LONDON_UTC_OFFSET"
    printf 'OK: silencer window: %s\n' "$SILENCER_WINDOW"
    printf 'OK: 6 Music -> silencer -> FIP duck/mix graph ran for %s seconds\n' "$check_seconds"
}

run_aws_pipeline_once() {
    ffmpeg \
      -hide_banner \
      -loglevel warning \
      $(ffmpeg_live_input_args) \
      -i "$BBC_URL" \
      -f s16le -ar "$SAMPLE_RATE" -ac 2 pipe:1 | \
    "$SILENCER" -t -x -v20 -s"$SAMPLE_RATE" -T "$START_TIME" -z "$LONDON_UTC_OFFSET" -w "$SILENCER_WINDOW" | \
    ffmpeg \
      -hide_banner \
      -loglevel warning \
      -re \
      -f s16le -ar "$SAMPLE_RATE" -ac 2 -i pipe:0 \
      $(ffmpeg_live_input_args) \
      -i "$FIP_URL" \
      -filter_complex "$(mix_with_fip_filter)" \
      -map '[out]' \
      -codec:a libmp3lame \
      -ar "$SAMPLE_RATE" \
      -ac 2 \
      -b:a "$AWS_AUDIO_BITRATE" \
      -write_xing 0 \
      -flush_packets 1 \
      -content_type audio/mpeg \
      -f mp3 "$AWS_ICECAST_URL"
}

run_aws_pipeline_forever() {
    while :; do
        START_TIME=$(get_stream_start_time)
        LONDON_UTC_OFFSET=$(get_london_utc_offset)

        printf 'BBC stream timestamp: %s\n' "$START_TIME" >&2
        printf 'London UTC offset: %s\n' "$LONDON_UTC_OFFSET" >&2
        printf 'Silencer window: %s\n' "$SILENCER_WINDOW" >&2

        if run_aws_pipeline_once; then
            printf 'AWS stream pipeline ended; restarting in %s seconds.\n' "$AWS_RESTART_DELAY_SECONDS" >&2
        else
            printf 'AWS stream pipeline failed; restarting in %s seconds.\n' "$AWS_RESTART_DELAY_SECONDS" >&2
        fi

        sleep "$AWS_RESTART_DELAY_SECONDS"
    done
}

CHECK_ONLY=0
AWS_MODE=0
while [ "$#" -gt 0 ]; do
    case "$1" in
        --check)
            CHECK_ONLY=1
            shift
            ;;
        -AWS)
            AWS_MODE=1
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
if [ "$AWS_MODE" -eq 0 ]; then
    require_command ffplay
fi
ensure_silencer

if [ "$AWS_MODE" -eq 1 ]; then
    AWS_ICECAST_URL=$(build_aws_icecast_url)
fi
if [ "$AWS_MODE" -eq 0 ] || [ "$CHECK_ONLY" -eq 1 ]; then
    START_TIME=$(get_stream_start_time)
    LONDON_UTC_OFFSET=$(get_london_utc_offset)
fi

if [ "$CHECK_ONLY" -eq 1 ]; then
    run_check
    exit 0
fi

printf 'BBC 6 Music: %s\n' "$BBC_URL" >&2
printf 'FIP: %s\n' "$FIP_URL" >&2
if [ "$AWS_MODE" -eq 1 ]; then
    printf 'AWS Icecast output: %s\n' "$(redact_url_password "$AWS_ICECAST_URL")" >&2
    printf 'AWS audio bitrate: %s\n' "$AWS_AUDIO_BITRATE" >&2
    printf 'AWS restart delay: %s seconds\n' "$AWS_RESTART_DELAY_SECONDS" >&2
else
    printf 'BBC stream timestamp: %s\n' "$START_TIME" >&2
    printf 'London UTC offset: %s\n' "$LONDON_UTC_OFFSET" >&2
fi
printf 'Silencer window: %s\n' "$SILENCER_WINDOW" >&2

if [ "$AWS_MODE" -eq 1 ]; then
    run_aws_pipeline_forever
else
    ffmpeg \
      -hide_banner \
      -loglevel warning \
      $(ffmpeg_live_input_args) \
      -i "$BBC_URL" \
      -f s16le -ar "$SAMPLE_RATE" -ac 2 pipe:1 | \
    "$SILENCER" -t -x -v20 -s"$SAMPLE_RATE" -T "$START_TIME" -z "$LONDON_UTC_OFFSET" -w "$SILENCER_WINDOW" | \
    ffmpeg \
      -hide_banner \
      -loglevel warning \
      -re \
      -f s16le -ar "$SAMPLE_RATE" -ac 2 -i pipe:0 \
      $(ffmpeg_live_input_args) \
      -i "$FIP_URL" \
      -filter_complex "$(mix_with_fip_filter)" \
      -map '[out]' \
      -f s16le -ar "$SAMPLE_RATE" -ac 2 pipe:1 | \
    ffplay -hide_banner -loglevel warning -nodisp -f s16le -ar "$SAMPLE_RATE" -ch_layout stereo -
fi
