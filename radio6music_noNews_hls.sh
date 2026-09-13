#!/bin/sh
set -eu

BBC_URL="${BBC_URL:-http://as-hls-ww-live.akamaized.net/pool_81827798/live/ww/bbc_6music/bbc_6music.isml/bbc_6music-audio%3d320000.norewind.m3u8}"
FIP_URL="${FIP_URL:-https://stream.radiofrance.fr/fip/fip_hifi.m3u8?id=radiofrance}"
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
SILENCER="$SCRIPT_DIR/silencer"
SAMPLE_RATE=48000
SILENCER_WINDOW="${NEWS_SCHEDULE:-$SCRIPT_DIR/news_schedule.ini}"

OUT_DIR="${OUT_DIR:-$SCRIPT_DIR/hls_radio6music_noNews}"
PLAYLIST="$OUT_DIR/radio6music_noNews.m3u8"
SEGMENT_PATTERN="$OUT_DIR/radio6music_noNews_%05d.ts"
LOG="$OUT_DIR/radio6music_noNews_hls.log"

FIP_VOLUME="${FIP_VOLUME:-0.85}"
DUCK_THRESHOLD="${DUCK_THRESHOLD:-0.002}"
DUCK_RATIO="${DUCK_RATIO:-20}"
FIP_FADE_OUT_MS="${FIP_FADE_OUT_MS:-700}"
FIP_FADE_IN_MS="${FIP_FADE_IN_MS:-1800}"
HLS_AUDIO_BITRATE="${HLS_AUDIO_BITRATE:-128k}"
HLS_AAC_CODER="${HLS_AAC_CODER:-fast}"
HLS_TIME="${HLS_TIME:-6}"
HLS_LIST_SIZE="${HLS_LIST_SIZE:-20}"
HLS_RESTART_DELAY_SECONDS="${HLS_RESTART_DELAY_SECONDS:-1}"
HLS_CLEAN_START="${HLS_CLEAN_START:-0}"

usage() {
    printf 'Usage: %s [--check] [-w ranges-or-file]\n' "$0"
    printf 'Writes a rolling HLS audio stream to: %s\n' "$PLAYLIST"
    printf 'Set OUT_DIR=... to write the HLS files elsewhere.\n'
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

stream_start_week_second() (
    # Convert the HLS timestamp to London local time. GNU date is used in the
    # container; the BSD branch keeps local macOS checks working too.
    if date --version >/dev/null 2>&1; then
        local_clock=$(TZ=Europe/London date -d "$START_TIME" '+%w %H %M %S')
    else
        timestamp=$(printf '%s\n' "$START_TIME" | sed 's/\.[0-9]*//; s/Z$/+0000/; s/\([+-][0-9][0-9]\):\([0-9][0-9]\)$/\1\2/')
        epoch=$(TZ=UTC date -j -f '%Y-%m-%dT%H:%M:%S%z' "$timestamp" '+%s')
        local_clock=$(TZ=Europe/London date -r "$epoch" '+%w %H %M %S')
    fi

    printf '%s\n' "$local_clock" | awk '{ print $1 * 86400 + $2 * 3600 + $3 * 60 + $4 }'
)

news_gate_expression_from_schedule() {
    schedule_file=$1
    start_week_second=$2
    fade_out_ms=$3

    awk -v base="$start_week_second" -v fade_out_ms="$fade_out_ms" '
        function trim(value) {
            sub(/^[[:space:]]+/, "", value)
            sub(/[[:space:]]+$/, "", value)
            return value
        }
        function fail(message) {
            print "Error: " message > "/dev/stderr"
            failed = 1
            exit 1
        }
        function add_condition(condition) {
            expression = expression (expression == "" ? "" : "+") condition
        }
        function add_window(start_second, end_second,    week, wrapped_start, wrapped_end) {
            week = 7 * 86400
            wrapped_start = start_second % week
            wrapped_end = end_second % week
            if (wrapped_start < 0) wrapped_start += week
            if (wrapped_end < 0) wrapped_end += week

            if (wrapped_start < wrapped_end) {
                add_condition(sprintf("between(mod(t+%d\\,604800)\\,%.3f\\,%.3f)", base, wrapped_start, wrapped_end))
            } else {
                add_condition(sprintf("between(mod(t+%d\\,604800)\\,%.3f\\,604800)", base, wrapped_start))
                add_condition(sprintf("between(mod(t+%d\\,604800)\\,0\\,%.3f)", base, wrapped_end))
            }
        }
        function parse_times(value, destination,    count, items, i, parts, hour, minute) {
            count = split(value, items, ",")
            for (i = 1; i <= count; ++i) {
                items[i] = trim(items[i])
                if (split(items[i], parts, ":") != 2 || parts[1] !~ /^[0-9]+$/ ||
                    parts[2] !~ /^[0-9]+$/ || parts[1] + 0 > 23 || parts[2] + 0 > 59)
                    fail("invalid news time in " FILENAME ": " items[i])
                hour = parts[1] + 0
                minute = parts[2] + 0
                if (destination == "weekday") weekday[++weekday_count] = hour * 60 + minute
                else weekend[++weekend_count] = hour * 60 + minute
            }
        }
        {
            line = $0
            sub(/[;#].*$/, "", line)
            line = trim(line)
            if (line == "") next
            if (line ~ /^\[[^]]+\]$/) {
                section = tolower(substr(line, 2, length(line) - 2))
                next
            }
            separator = index(line, "=")
            if (!separator) fail("invalid line in news schedule: " line)
            key = tolower(trim(substr(line, 1, separator - 1)))
            value = trim(substr(line, separator + 1))
            if (section == "window" && (key == "before_minutes" || key == "before")) before = value + 0
            else if (section == "window" && (key == "after_minutes" || key == "after")) after = value + 0
            else if (section == "weekday" && key == "times") parse_times(value, "weekday")
            else if (section == "weekend" && key == "times") parse_times(value, "weekend")
            else fail("unsupported news schedule entry: " line)
        }
        END {
            if (failed) exit 1
            if (before == "" || after == "" || before < 0 || after < 0 || before + after <= 0 ||
                !weekday_count || !weekend_count)
                fail("incomplete news schedule: " FILENAME)

            # date +%w uses Sunday=0, matching these day numbers.
            for (day = 1; day <= 5; ++day)
                for (i = 1; i <= weekday_count; ++i)
                    add_window(day * 86400 + (weekday[i] - before) * 60,
                               day * 86400 + (weekday[i] + after) * 60 + fade_out_ms / 1000)
            for (day = 0; day <= 6; day += 6)
                for (i = 1; i <= weekend_count; ++i)
                    add_window(day * 86400 + (weekend[i] - before) * 60,
                               day * 86400 + (weekend[i] + after) * 60 + fade_out_ms / 1000)

            print "min(1\\," expression ")"
        }
    ' "$schedule_file"
}

news_gate_expression_from_ranges() {
    ranges=$1
    start_week_second=$2
    fade_out_ms=$3

    printf '%s\n' "$ranges" | awk -v base="$start_week_second" -v fade_out_ms="$fade_out_ms" '
        function fail(message) {
            print "Error: " message > "/dev/stderr"
            exit 1
        }
        function add(condition) {
            expression = expression (expression == "" ? "" : "+") condition
        }
        BEGIN { FS = "," }
        {
            for (i = 1; i <= NF; ++i) {
                if (split($i, limits, "-") != 2 || limits[1] !~ /^[0-9]+$/ || limits[2] !~ /^[0-9]+$/)
                    fail("invalid minute range: " $i)
                start = limits[1] + 0
                end = limits[2] + 0
                if (start > 59 || end > 59 || start == end) fail("invalid minute range: " $i)
                if (start < end)
                    add(sprintf("between(mod(t+%d\\,3600)\\,%d\\,%.3f)", base, start * 60, end * 60 + fade_out_ms / 1000))
                else {
                    add(sprintf("between(mod(t+%d\\,3600)\\,%d\\,3600)", base, start * 60))
                    add(sprintf("between(mod(t+%d\\,3600)\\,0\\,%.3f)", base, end * 60 + fade_out_ms / 1000))
                }
            }
        }
        END {
            if (expression == "") fail("no minute ranges supplied")
            print "min(1\\," expression ")"
        }
    '
}

build_news_gate_expression() {
    start_week_second=$(stream_start_week_second)
    if [ -f "$SILENCER_WINDOW" ]; then
        news_gate_expression_from_schedule "$SILENCER_WINDOW" "$start_week_second" "$FIP_FADE_OUT_MS"
    else
        news_gate_expression_from_ranges "$SILENCER_WINDOW" "$start_week_second" "$FIP_FADE_OUT_MS"
    fi
}

mix_with_fip_filter() {
    printf '[0:a]aformat=sample_fmts=fltp:channel_layouts=stereo,asplit=2[bbc][sc];'
    printf '[1:a]aresample=%s,aformat=sample_fmts=fltp:channel_layouts=stereo,volume=%s[fip];' "$SAMPLE_RATE" "$FIP_VOLUME"
    printf '[fip][sc]sidechaincompress=threshold=%s:ratio=%s:attack=%s:release=%s:makeup=1:link=maximum:detection=rms[fipduck];' "$DUCK_THRESHOLD" "$DUCK_RATIO" "$FIP_FADE_OUT_MS" "$FIP_FADE_IN_MS"
    printf "[fipduck]volume='%s':eval=frame[fipwindowed];" "$NEWS_GATE_EXPRESSION"
    printf '[bbc][fipwindowed]amix=inputs=2:duration=first:normalize=0:dropout_transition=0,alimiter=limit=0.95,aresample=async=1000:first_pts=0[out]'
}

ensure_output_dir() {
    mkdir -p "$OUT_DIR"
}

clean_output_dir() {
    ensure_output_dir
    rm -f "$PLAYLIST" "$OUT_DIR"/radio6music_noNews_*.ts "$LOG"
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
    NEWS_GATE_EXPRESSION=$(build_news_gate_expression)

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
    "$SILENCER" -e -t -x -v20 -s"$SAMPLE_RATE" -T "$START_TIME" -z "$LONDON_UTC_OFFSET" -w "$SILENCER_WINDOW" 2>>"$LOG" | \
    ffmpeg \
      -hide_banner \
      -loglevel warning \
      -re \
      -f s16le -ar "$SAMPLE_RATE" -ac 2 -i pipe:0 \
      $(ffmpeg_live_input_args) \
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
}

run_check() {
    clean_output_dir
    run_pipeline 12

    if [ ! -s "$PLAYLIST" ]; then
        printf 'Error: HLS playlist was not created: %s\n' "$PLAYLIST" >&2
        exit 1
    fi

    if ! ls "$OUT_DIR"/radio6music_noNews_*.ts >/dev/null 2>&1; then
        printf 'Error: HLS segments were not created in: %s\n' "$OUT_DIR" >&2
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
        printf 'Silencer window: %s\n' "$SILENCER_WINDOW" >&2
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

if [ "${SKIPPER_HLS_FUNCTIONS_ONLY:-0}" = "1" ]; then
    return 0
fi

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
require_command awk
require_command date
require_command ffmpeg
ensure_silencer

if [ "$CHECK_ONLY" -eq 1 ]; then
    START_TIME=$(get_stream_start_time)
    LONDON_UTC_OFFSET=$(get_london_utc_offset)
    run_check
    exit 0
fi

printf 'Writing HLS stream to: %s\n' "$PLAYLIST"
printf 'Log: %s\n' "$LOG"
printf 'Silencer window: %s\n' "$SILENCER_WINDOW"
printf 'Keep this script running while you listen.\n'
case "$HLS_CLEAN_START" in
    1|true|TRUE|yes|YES)
        clean_output_dir
        ;;
esac
run_pipeline_forever
