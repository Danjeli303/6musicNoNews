#!/bin/sh
set -eu

BBC_URL="${BBC_URL:-http://as-hls-ww-live.akamaized.net/pool_81827798/live/ww/bbc_6music/bbc_6music.isml/bbc_6music-audio%3d320000.norewind.m3u8}"
FIP_URL="${FIP_URL:-https://stream.radiofrance.fr/fip/fip_hifi.m3u8?id=radiofrance}"
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
SILENCER="$SCRIPT_DIR/silencer"
SAMPLE_RATE=48000

# FIP is ducked by the silenced 6 Music signal. Release controls FIP fade-in
# when 6 Music becomes silent; attack controls FIP fade-out when 6 Music returns.
FIP_VOLUME="${FIP_VOLUME:-0.85}"
DUCK_THRESHOLD="${DUCK_THRESHOLD:-0.002}"
DUCK_RATIO="${DUCK_RATIO:-20}"
FIP_FADE_OUT_MS="${FIP_FADE_OUT_MS:-700}"
FIP_FADE_IN_MS="${FIP_FADE_IN_MS:-1800}"
AWS_ICECAST_URL="${AWS_ICECAST_URL:-icecast://source:hackme@icecast:8000/the-radio.mp3}"
AWS_AUDIO_BITRATE="${AWS_AUDIO_BITRATE:-128k}"

usage() {
    printf 'Usage: %s [--check|-AWS]\n' "$0"
    printf 'Set BBC_URL=... or FIP_URL=... to override either stream.\n'
    printf 'With -AWS, set AWS_ICECAST_URL=icecast://source:password@host:8000/mount.mp3 if needed.\n'
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

run_check() {
    check_seconds=3

    ffmpeg \
      -hide_banner \
      -loglevel warning \
      -reconnect 1 \
      -reconnect_streamed 1 \
      -reconnect_delay_max 2 \
      -i "$BBC_URL" \
      -t "$check_seconds" \
      -f s16le -ar "$SAMPLE_RATE" -ac 2 pipe:1 | \
    "$SILENCER" -t -x -q -s"$SAMPLE_RATE" -T "$START_TIME" -z "$LONDON_UTC_OFFSET" | \
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
      -t "$check_seconds" \
      -f null -

    printf 'OK: BBC stream timestamp: %s\n' "$START_TIME"
    printf 'OK: London UTC offset: %s\n' "$LONDON_UTC_OFFSET"
    printf 'OK: 6 Music -> silencer -> FIP duck/mix graph ran for %s seconds\n' "$check_seconds"
}

CHECK_ONLY=0
AWS_MODE=0
case "${1:-}" in
    "")
        ;;
    --check)
        CHECK_ONLY=1
        ;;
    -AWS)
        AWS_MODE=1
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
if [ "$AWS_MODE" -eq 0 ]; then
    require_command ffplay
fi
require_command make
ensure_silencer

START_TIME=$(get_stream_start_time)
LONDON_UTC_OFFSET=$(get_london_utc_offset)

if [ "$CHECK_ONLY" -eq 1 ]; then
    run_check
    exit 0
fi

printf 'BBC 6 Music: %s\n' "$BBC_URL" >&2
printf 'FIP: %s\n' "$FIP_URL" >&2
printf 'BBC stream timestamp: %s\n' "$START_TIME" >&2
printf 'London UTC offset: %s\n' "$LONDON_UTC_OFFSET" >&2
if [ "$AWS_MODE" -eq 1 ]; then
    printf 'AWS Icecast output: %s\n' "$AWS_ICECAST_URL" >&2
    printf 'AWS audio bitrate: %s\n' "$AWS_AUDIO_BITRATE" >&2
fi

if [ "$AWS_MODE" -eq 1 ]; then
    ffmpeg \
      -hide_banner \
      -loglevel warning \
      -reconnect 1 \
      -reconnect_streamed 1 \
      -reconnect_delay_max 2 \
      -i "$BBC_URL" \
      -f s16le -ar "$SAMPLE_RATE" -ac 2 pipe:1 | \
    "$SILENCER" -t -x -v20 -s"$SAMPLE_RATE" -T "$START_TIME" -z "$LONDON_UTC_OFFSET" | \
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
      -codec:a libmp3lame \
      -b:a "$AWS_AUDIO_BITRATE" \
      -content_type audio/mpeg \
      -f mp3 "$AWS_ICECAST_URL"
else
    ffmpeg \
      -hide_banner \
      -loglevel warning \
      -reconnect 1 \
      -reconnect_streamed 1 \
      -reconnect_delay_max 2 \
      -i "$BBC_URL" \
      -f s16le -ar "$SAMPLE_RATE" -ac 2 pipe:1 | \
    "$SILENCER" -t -x -v20 -s"$SAMPLE_RATE" -T "$START_TIME" -z "$LONDON_UTC_OFFSET" | \
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
      -f s16le -ar "$SAMPLE_RATE" -ac 2 pipe:1 | \
    ffplay -hide_banner -loglevel warning -nodisp -f s16le -ar "$SAMPLE_RATE" -ch_layout stereo -
fi
