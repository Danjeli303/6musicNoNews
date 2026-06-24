#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
CASE_MANIFEST="${CASE_MANIFEST:-$SCRIPT_DIR/listening-cases.tsv}"
FIXTURE_DIR="${FIXTURE_DIR:-$SCRIPT_DIR/listening-fixtures}"

MAIN_INPUT="${MAIN_INPUT:-/Users/danielschembri/Desktop/iPlayer Recordings/Gilles_Peterson_-_A_tribute_to_Abdullah_Ibrahim_and_All_Winners_so_far_m002xmyp_original.m4a}"
FADE_BED_SOURCE_KEY="${FADE_BED_SOURCE_KEY:-main}"
FADE_BED_OFFSET_SECONDS="${FADE_BED_OFFSET_SECONDS:-3120}"
FADE_BED_DURATION_SECONDS="${FADE_BED_DURATION_SECONDS:-1200}"
FADE_BED_CACHE="${FADE_BED_CACHE:-$FIXTURE_DIR/source-cache/non-news-fade-bed.m4a}"

SAMPLE_RATE="${SAMPLE_RATE:-48000}"
SKIPPER_BITRATE="${SKIPPER_BITRATE:-192k}"
HLS_AUDIO_BITRATE="${HLS_AUDIO_BITRATE:-128k}"
HLS_TIME="${HLS_TIME:-6}"
FADE_BED_VOLUME="${FADE_BED_VOLUME:-0.85}"
DUCK_THRESHOLD="${DUCK_THRESHOLD:-0.002}"
DUCK_RATIO="${DUCK_RATIO:-20}"
FADE_BED_FADE_OUT_MS="${FADE_BED_FADE_OUT_MS:-700}"
FADE_BED_FADE_IN_MS="${FADE_BED_FADE_IN_MS:-1800}"

SKIPPER="$SCRIPT_DIR/skipper"
SILENCER="$SCRIPT_DIR/silencer"

usage() {
    cat <<EOF
Usage: $0 <command> [case|all]

Commands:
  list                 Show available listening cases.
  check                Verify tools, source metadata, and case bounds.
  check-wrappers       Smoke test existing recorded wrappers; live checks need RUN_LIVE_CHECKS=1.
  test-fixtures        Verify generated fixtures, durations, and processing logs.
  generate CASE|all    Regenerate local skipper and silencer HLS fixtures.
  logs CASE            Print skipper and silencer fixture logs for a case.
  silencer CASE        Stream a case through silencer to ffplay.
  hls CASE             Play a generated HLS fixture with ffplay.
  skipper CASE         Play a generated skipper .m4a fixture with ffplay.

Environment:
  MAIN_INPUT=...       Override the source recording.
  FADE_BED_CACHE=...   Override the local non-news bed used for fixture generation.
EOF
}

require_command() {
    if ! command -v "$1" >/dev/null 2>&1; then
        printf 'Error: required command not found: %s\n' "$1" >&2
        exit 1
    fi
}

ensure_skipper() {
    if [ ! -x "$SKIPPER" ]; then
        require_command make
        make -C "$SCRIPT_DIR" skipper
    fi

    if [ ! -x "$SKIPPER" ]; then
        printf 'Error: skipper executable was not found at %s\n' "$SKIPPER" >&2
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

case_ids() {
    awk -F '\t' 'NR > 1 && $1 != "" { print $1 }' "$CASE_MANIFEST"
}

source_path_for_key() {
    case "$1" in
        main) printf '%s\n' "$MAIN_INPUT" ;;
        *)
            printf 'Error: unknown source key in manifest: %s\n' "$1" >&2
            exit 1
            ;;
    esac
}

load_case() {
    case_line=$(awk -F '\t' -v id="$1" 'NR > 1 && $1 == id { print; found = 1 } END { if (!found) exit 1 }' "$CASE_MANIFEST") || {
        printf 'Error: unknown case: %s\n' "$1" >&2
        printf 'Run "%s list" to see available cases.\n' "$0" >&2
        exit 1
    }

    old_ifs=$IFS
    IFS='	'
    # shellcheck disable=SC2034
    set -- $case_line
    IFS=$old_ifs

    CASE_ID=$1
    CASE_TYPE=$2
    SOURCE_KEY=$3
    CASE_START_ISO=$4
    EXPECTED_PROGRAMME_START_ISO=$5
    OFFSET_SECONDS=$6
    DURATION_SECONDS=$7
    INPUT_START_ISO=$8
    INPUT_OFFSET_SECONDS=$9
    INPUT_DURATION_SECONDS=${10}
    WINDOW=${11}
    PURPOSE=${12}
    INPUT=$(source_path_for_key "$SOURCE_KEY")
    UTC_OFFSET=$(offset_from_timestamp "$INPUT_START_ISO")
}

offset_from_timestamp() {
    case "$1" in
        *[+-][0-9][0-9]:[0-9][0-9])
            printf '%s\n' "$1" | sed 's/^.*\([+-][0-9][0-9]:[0-9][0-9]\)$/\1/'
            ;;
        *Z|*z)
            printf '+00:00\n'
            ;;
        *)
            printf 'Error: timestamp does not include a UTC offset: %s\n' "$1" >&2
            exit 1
            ;;
    esac
}

recorded_start_time() {
    ffprobe \
      -hide_banner \
      -loglevel warning \
      -show_entries format_tags=date \
      -of default=noprint_wrappers=1:nokey=1 \
      "$1" | head -n 1
}

recorded_duration() {
    ffprobe \
      -hide_banner \
      -loglevel warning \
      -show_entries format=duration \
      -of default=noprint_wrappers=1:nokey=1 \
      "$1" | head -n 1
}

media_duration() {
    ffprobe \
      -hide_banner \
      -loglevel error \
      -show_entries format=duration \
      -of default=noprint_wrappers=1:nokey=1 \
      "$1" | head -n 1
}

playlist_duration() {
    awk -F ':' '
        /^#EXTINF:/ {
            split($2, parts, ",")
            total += parts[1]
        }
        END {
            printf "%.3f\n", total
        }
    ' "$1"
}

assert_file_nonempty() {
    if [ ! -s "$1" ]; then
        printf 'Error: missing or empty fixture: %s\n' "$1" >&2
        exit 1
    fi
}

assert_duration_between() {
    path=$1
    actual=$2
    min=$3
    max=$4

    if ! awk -v actual="$actual" -v min="$min" -v max="$max" 'BEGIN { exit !(actual >= min && actual <= max) }'; then
        printf 'Error: %s duration %.3fs outside expected range %.3fs..%.3fs\n' "$path" "$actual" "$min" "$max" >&2
        exit 1
    fi
}

assert_log_contains() {
    log=$1
    pattern=$2
    label=$3

    if ! grep -E "$pattern" "$log" >/dev/null 2>&1; then
        printf 'Error: %s missing expected log evidence: %s\n' "$log" "$label" >&2
        exit 1
    fi
}

case_fits_source() {
    duration=$(recorded_duration "$INPUT")
    awk -v offset="$INPUT_OFFSET_SECONDS" -v length="$INPUT_DURATION_SECONDS" -v duration="$duration" \
      'BEGIN { exit !((offset + length) <= duration) }'
}

skipper_fixture_path() {
    printf '%s/skipper/%s_skipper.m4a\n' "$FIXTURE_DIR" "$CASE_ID"
}

skipper_log_path() {
    printf '%s/skipper/%s_skipper.log\n' "$FIXTURE_DIR" "$CASE_ID"
}

hls_dir_path() {
    printf '%s/silencer-hls/%s\n' "$FIXTURE_DIR" "$CASE_ID"
}

hls_playlist_path() {
    printf '%s/radio6music_noNews.m3u8\n' "$(hls_dir_path)"
}

hls_log_path() {
    printf '%s/radio6music_noNews_hls.log\n' "$(hls_dir_path)"
}

ensure_fade_bed_cache() {
    if [ -s "$FADE_BED_CACHE" ]; then
        return
    fi

    bed_input=$(source_path_for_key "$FADE_BED_SOURCE_KEY")
    mkdir -p "$(dirname "$FADE_BED_CACHE")"
    printf 'Generating non-news fade bed: %s\n' "$FADE_BED_CACHE"
    ffmpeg \
      -hide_banner \
      -loglevel warning \
      -y \
      -ss "$FADE_BED_OFFSET_SECONDS" \
      -i "$bed_input" \
      -vn \
      -t "$FADE_BED_DURATION_SECONDS" \
      -c:a aac -b:a "$HLS_AUDIO_BITRATE" \
      "$FADE_BED_CACHE"
}

mix_with_fip_filter() {
    printf '[0:a]aformat=sample_fmts=fltp:channel_layouts=stereo,asplit=2[bbc][sc];'
    printf '[1:a]aresample=%s,aformat=sample_fmts=fltp:channel_layouts=stereo,volume=%s[bed];' "$SAMPLE_RATE" "$FADE_BED_VOLUME"
    printf '[bed][sc]sidechaincompress=threshold=%s:ratio=%s:attack=%s:release=%s:makeup=1:link=maximum:detection=rms[bedduck];' "$DUCK_THRESHOLD" "$DUCK_RATIO" "$FADE_BED_FADE_OUT_MS" "$FADE_BED_FADE_IN_MS"
    printf '[bbc][bedduck]amix=inputs=2:duration=first:normalize=0:dropout_transition=0,alimiter=limit=0.95,aresample=async=1000:first_pts=0[out]'
}

list_cases() {
    printf '%-16s %-8s %-8s %-21s %-6s %s\n' "CASE" "TYPE" "SOURCE" "WINDOW" "SECS" "FIXTURES"
    while IFS='	' read -r case_id case_type source_key case_start expected offset duration input_start input_offset input_duration window purpose; do
        [ "$case_id" = "case_id" ] && continue
        [ -z "$case_id" ] && continue
        printf '%-16s %-8s %-8s %-21s %-6s %s, %s\n' \
            "$case_id" "$case_type" "$source_key" "$window" "$input_duration" \
            "$FIXTURE_DIR/skipper/${case_id}_skipper.m4a" \
            "$FIXTURE_DIR/silencer-hls/$case_id/radio6music_noNews.m3u8"
    done < "$CASE_MANIFEST"
}

check_sources_and_cases() {
    require_command awk
    require_command ffmpeg
    require_command ffprobe
    require_command sed
    ensure_skipper
    ensure_silencer

    if [ ! -f "$CASE_MANIFEST" ]; then
        printf 'Error: case manifest not found: %s\n' "$CASE_MANIFEST" >&2
        exit 1
    fi

    for case_id in $(case_ids); do
        load_case "$case_id"

        if [ ! -f "$INPUT" ]; then
            printf 'Error: input file not found for %s: %s\n' "$CASE_ID" "$INPUT" >&2
            exit 1
        fi

        actual_start=$(recorded_start_time "$INPUT")
        if [ "$actual_start" != "$EXPECTED_PROGRAMME_START_ISO" ]; then
            printf 'Error: %s expected programme start %s, got %s\n' "$CASE_ID" "$EXPECTED_PROGRAMME_START_ISO" "$actual_start" >&2
            exit 1
        fi

        if ! case_fits_source; then
            printf 'Error: %s does not fit inside source recording\n' "$CASE_ID" >&2
            exit 1
        fi

        case "$CASE_TYPE:$CASE_ID" in
            news:news_section|music:music_section)
                ;;
            *)
                printf 'Error: expected fixtures named news_section and music_section, got %s:%s\n' "$CASE_TYPE" "$CASE_ID" >&2
                exit 1
                ;;
        esac
    done

    first_case=$(case_ids | head -n 1)
    load_case "$first_case"
    ffmpeg \
      -hide_banner \
      -loglevel warning \
      -y \
      -ss "$INPUT_OFFSET_SECONDS" \
      -i "$INPUT" \
      -vn \
      -t 1 \
      -f s16le -ar "$SAMPLE_RATE" -ac 2 - | \
    "$SILENCER" -p -q -s"$SAMPLE_RATE" -T "$INPUT_START_ISO" -z "$UTC_OFFSET" >/dev/null

    printf 'OK: checked %s cases, source metadata, bounds, and short decode/process path\n' "$(case_ids | wc -l | tr -d ' ')"
}

check_wrappers() {
    require_command ffmpeg
    require_command ffprobe
    "$SCRIPT_DIR/skip_6music_news.sh" --check "$MAIN_INPUT"
    "$SCRIPT_DIR/silence_6music_news.sh" --check "$MAIN_INPUT"

    case "${RUN_LIVE_CHECKS:-0}" in
        1|true|TRUE|yes|YES)
            "$SCRIPT_DIR/play_6music_silencer.sh" --check
            "$SCRIPT_DIR/radio6music_noNews_hls.sh" --check
            ;;
        *)
            printf 'SKIP: live wrapper checks disabled; set RUN_LIVE_CHECKS=1 to run them.\n'
            ;;
    esac
}

test_fixtures() {
    require_command awk
    require_command ffprobe
    require_command grep

    for case_id in $(case_ids); do
        load_case "$case_id"

        skipper_output=$(skipper_fixture_path)
        skipper_log=$(skipper_log_path)
        playlist=$(hls_playlist_path)
        silencer_log=$(hls_log_path)

        assert_file_nonempty "$skipper_output"
        assert_file_nonempty "$skipper_log"
        assert_file_nonempty "$playlist"
        assert_file_nonempty "$silencer_log"

        segment_count=$(find "$(hls_dir_path)" -maxdepth 1 -type f -name 'radio6music_noNews_*.ts' | wc -l | tr -d ' ')
        if [ "$segment_count" -lt 1 ]; then
            printf 'Error: no HLS segments found for %s in %s\n' "$CASE_ID" "$(hls_dir_path)" >&2
            exit 1
        fi

        playlist_secs=$(playlist_duration "$playlist")
        min_playlist=$(awk -v secs="$INPUT_DURATION_SECONDS" 'BEGIN { printf "%.3f\n", secs - 8 }')
        max_playlist=$(awk -v secs="$INPUT_DURATION_SECONDS" 'BEGIN { printf "%.3f\n", secs + 8 }')
        assert_duration_between "$playlist" "$playlist_secs" "$min_playlist" "$max_playlist"

        skipper_secs=$(media_duration "$skipper_output")
        assert_log_contains "$skipper_log" 'audio written =' 'skipper final summary'
        assert_log_contains "$silencer_log" 'Debug time:' 'silencer clock alignment'
        assert_log_contains "$silencer_log" 'Passed [0-9]+ samples' 'silencer pass-through decisions'

        case "$CASE_TYPE" in
            news)
                assert_duration_between "$skipper_output" "$skipper_secs" 1 20
                assert_log_contains "$skipper_log" 'audio discarded = .*9[0-9]\.[0-9]%\)' 'news mostly skipped'
                assert_log_contains "$silencer_log" 'Fade OUT to SILENCE' 'scheduled fade-out to silence'
                assert_log_contains "$silencer_log" 'Silenced [0-9]+ samples' 'scheduled silencing'
                ;;
            music)
                min_skipper=$(awk -v secs="$INPUT_DURATION_SECONDS" 'BEGIN { printf "%.3f\n", secs * 0.85 }')
                max_skipper=$(awk -v secs="$INPUT_DURATION_SECONDS" 'BEGIN { printf "%.3f\n", secs + 8 }')
                assert_duration_between "$skipper_output" "$skipper_secs" "$min_skipper" "$max_skipper"
                assert_log_contains "$skipper_log" 'audio written = .*9[0-9]\.[0-9]%\)' 'music mostly preserved'
                ;;
            *)
                printf 'Error: unknown case type in manifest: %s\n' "$CASE_TYPE" >&2
                exit 1
                ;;
        esac

        printf 'OK: %s fixtures: skipper %.1fs, HLS %.1fs, %s segments\n' \
            "$CASE_ID" "$skipper_secs" "$playlist_secs" "$segment_count"
    done
}

generate_skipper_case() {
    output=$(skipper_fixture_path)
    log=$(skipper_log_path)
    mkdir -p "$(dirname "$output")"

    printf 'Generating skipper fixture: %s\n' "$output"
    ffmpeg \
      -hide_banner \
      -loglevel warning \
      -y \
      -ss "$INPUT_OFFSET_SECONDS" \
      -i "$INPUT" \
      -vn \
      -t "$INPUT_DURATION_SECONDS" \
      -f s16le -ar "$SAMPLE_RATE" -ac 2 pipe:1 2>"$log" | \
    "$SKIPPER" -t -x -v20 -s"$SAMPLE_RATE" -T "$INPUT_START_ISO" -z "$UTC_OFFSET" 2>>"$log" | \
    ffmpeg \
      -hide_banner \
      -loglevel warning \
      -y \
      -f s16le -ar "$SAMPLE_RATE" -ac 2 -i pipe:0 \
      -c:a aac -b:a "$SKIPPER_BITRATE" \
      "$output" 2>>"$log"
}

generate_hls_case() {
    hls_dir=$(hls_dir_path)
    playlist=$(hls_playlist_path)
    log="$hls_dir/radio6music_noNews_hls.log"
    segment_pattern="$hls_dir/radio6music_noNews_%05d.ts"

    ensure_fade_bed_cache
    mkdir -p "$hls_dir"
    rm -f "$playlist" "$hls_dir"/radio6music_noNews_*.ts "$log"

    printf 'Generating silencer HLS fixture: %s\n' "$playlist"
    ffmpeg \
      -hide_banner \
      -loglevel warning \
      -y \
      -ss "$INPUT_OFFSET_SECONDS" \
      -i "$INPUT" \
      -vn \
      -t "$INPUT_DURATION_SECONDS" \
      -f s16le -ar "$SAMPLE_RATE" -ac 2 pipe:1 2>"$log" | \
    "$SILENCER" -t -x -v20 -s"$SAMPLE_RATE" -T "$INPUT_START_ISO" -z "$UTC_OFFSET" 2>>"$log" | \
    ffmpeg \
      -hide_banner \
      -loglevel warning \
      -y \
      -f s16le -ar "$SAMPLE_RATE" -ac 2 -i pipe:0 \
      -stream_loop -1 \
      -i "$FADE_BED_CACHE" \
      -filter_complex "$(mix_with_fip_filter)" \
      -map '[out]' \
      -c:a aac \
      -b:a "$HLS_AUDIO_BITRATE" \
      -f hls \
      -hls_time "$HLS_TIME" \
      -hls_list_size 0 \
      -start_number 0 \
      -hls_flags independent_segments \
      -hls_segment_filename "$segment_pattern" \
      "$playlist" 2>>"$log"
}

generate_case() {
    load_case "$1"
    ensure_skipper
    ensure_silencer
    generate_skipper_case
    generate_hls_case
}

generate_cases() {
    target=${1:-}
    if [ "$target" = "all" ]; then
        for case_id in $(case_ids); do
            generate_case "$case_id"
        done
    else
        [ -n "$target" ] || {
            printf 'Error: generate requires CASE or all\n' >&2
            exit 1
        }
        generate_case "$target"
    fi
}

play_silencer_case() {
    load_case "$1"
    require_command ffplay
    ensure_silencer

    ffmpeg \
      -hide_banner \
      -loglevel warning \
      -ss "$INPUT_OFFSET_SECONDS" \
      -i "$INPUT" \
      -vn \
      -t "$INPUT_DURATION_SECONDS" \
      -f s16le -ar "$SAMPLE_RATE" -ac 2 pipe:1 | \
    "$SILENCER" -t -x -v20 -s"$SAMPLE_RATE" -T "$INPUT_START_ISO" -z "$UTC_OFFSET" | \
    ffplay -hide_banner -loglevel warning -nodisp -f s16le -ar "$SAMPLE_RATE" -ch_layout stereo -
}

play_hls_case() {
    load_case "$1"
    require_command ffplay
    playlist=$(hls_playlist_path)

    if [ ! -s "$playlist" ]; then
        printf 'Error: HLS fixture not found: %s\n' "$playlist" >&2
        printf 'Run "%s generate %s" first.\n' "$0" "$CASE_ID" >&2
        exit 1
    fi

    ffplay -hide_banner -loglevel warning -nodisp "$playlist"
}

play_skipper_case() {
    load_case "$1"
    require_command ffplay
    output=$(skipper_fixture_path)

    if [ ! -s "$output" ]; then
        printf 'Error: skipper fixture not found: %s\n' "$output" >&2
        printf 'Run "%s generate %s" first.\n' "$0" "$CASE_ID" >&2
        exit 1
    fi

    ffplay -hide_banner -loglevel warning -nodisp "$output"
}

print_case_logs() {
    load_case "$1"
    skipper_log=$(skipper_log_path)
    silencer_log=$(hls_log_path)

    if [ ! -s "$skipper_log" ] && [ ! -s "$silencer_log" ]; then
        printf 'Error: no logs found for %s\n' "$CASE_ID" >&2
        printf 'Run "%s generate %s" first.\n' "$0" "$CASE_ID" >&2
        exit 1
    fi

    if [ -s "$skipper_log" ]; then
        printf '===== skipper log: %s =====\n' "$skipper_log"
        cat "$skipper_log"
    else
        printf 'MISSING: %s\n' "$skipper_log" >&2
    fi

    if [ -s "$silencer_log" ]; then
        printf '===== silencer HLS log: %s =====\n' "$silencer_log"
        cat "$silencer_log"
    else
        printf 'MISSING: %s\n' "$silencer_log" >&2
    fi
}

command=${1:-}
case "$command" in
    list)
        list_cases
        ;;
    check)
        check_sources_and_cases
        ;;
    check-wrappers)
        check_wrappers
        ;;
    test-fixtures)
        test_fixtures
        ;;
    generate)
        require_command ffmpeg
        require_command ffprobe
        generate_cases "${2:-}"
        ;;
    logs)
        [ "${2:-}" != "" ] || { printf 'Error: logs requires CASE\n' >&2; exit 1; }
        print_case_logs "$2"
        ;;
    silencer)
        [ "${2:-}" != "" ] || { printf 'Error: silencer requires CASE\n' >&2; exit 1; }
        play_silencer_case "$2"
        ;;
    hls)
        [ "${2:-}" != "" ] || { printf 'Error: hls requires CASE\n' >&2; exit 1; }
        play_hls_case "$2"
        ;;
    skipper)
        [ "${2:-}" != "" ] || { printf 'Error: skipper requires CASE\n' >&2; exit 1; }
        play_skipper_case "$2"
        ;;
    -h|--help|help)
        usage
        ;;
    "")
        usage
        exit 1
        ;;
    *)
        usage >&2
        exit 1
        ;;
esac
