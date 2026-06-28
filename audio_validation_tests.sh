#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
SAMPLE_RATE="${SAMPLE_RATE:-48000}"
CHANNELS=2
SLICE_SECONDS=20
PROGRAMME_START="2026-06-20T15:31:00+01:00"
PROGRAMME_OFFSET="+01:00"
MUSIC_START="2026-06-20T15:52:00+01:00"

if [ "${AUDIO_TEST_DIR:-}" ]; then
    WORK_DIR=$AUDIO_TEST_DIR
    CLEAN_WORK_DIR=0
    mkdir -p "$WORK_DIR"
else
    WORK_DIR=$(mktemp -d "${TMPDIR:-/tmp}/skipper-audio-tests.XXXXXX")
    CLEAN_WORK_DIR=1
fi

cleanup() {
    if [ "$CLEAN_WORK_DIR" -eq 1 ] && [ "${KEEP_AUDIO_TEST_DIR:-0}" != "1" ]; then
        rm -rf "$WORK_DIR"
    fi
}
trap cleanup EXIT HUP INT TERM

SOURCE="$WORK_DIR/source.m4a"
SILENCER="$SCRIPT_DIR/silencer"

require_command() {
    if ! command -v "$1" >/dev/null 2>&1; then
        printf 'Error: required command not found: %s\n' "$1" >&2
        exit 1
    fi
}

assert_file_nonempty() {
    if [ ! -s "$1" ]; then
        printf 'Error: missing or empty file: %s\n' "$1" >&2
        exit 1
    fi
}

assert_log_contains() {
    log=$1
    pattern=$2
    label=$3

    if ! grep -E "$pattern" "$log" >/dev/null 2>&1; then
        printf 'Error: %s missing %s\n' "$log" "$label" >&2
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

assert_pcm_all_zero() {
    if ! od -An -v -t d2 "$1" | awk '{ for (i = 1; i <= NF; ++i) if ($i != 0) exit 1 }'; then
        printf 'Error: expected silent PCM output: %s\n' "$1" >&2
        exit 1
    fi
}

assert_pcm_has_signal() {
    if ! od -An -v -t d2 "$1" | awk '{ for (i = 1; i <= NF; ++i) if ($i != 0) found = 1 } END { exit !found }'; then
        printf 'Error: expected audible PCM output: %s\n' "$1" >&2
        exit 1
    fi
}

assert_equals() {
    actual=$1
    expected=$2
    label=$3

    if [ "$actual" != "$expected" ]; then
        printf 'Error: %s expected "%s", got "%s"\n' "$label" "$expected" "$actual" >&2
        exit 1
    fi
}

assert_output_contains() {
    output_file=$1
    pattern=$2
    label=$3

    if ! grep -E "$pattern" "$output_file" >/dev/null 2>&1; then
        printf 'Error: %s missing %s\n' "$output_file" "$label" >&2
        exit 1
    fi
}

assert_single_progress_line() {
    output_file=$1
    count=$(tr '\r' '\n' < "$output_file" | grep -E '^Processed: [0-9]+%' | wc -l | awk '{ print $1 }')
    newline_count=$(grep -E '^Processed: [0-9]+%' "$output_file" | wc -l | awk '{ print $1 }')

    if [ "$count" -lt 1 ]; then
        printf 'Error: %s missing carriage-return progress output\n' "$output_file" >&2
        exit 1
    fi

    if [ "$newline_count" -gt 1 ]; then
        printf 'Error: %s printed %s newline progress lines instead of rewriting one line\n' "$output_file" "$newline_count" >&2
        exit 1
    fi
}

ffprobe_value() {
    ffprobe \
      -hide_banner \
      -loglevel error \
      -show_entries "$1" \
      -of default=noprint_wrappers=1:nokey=1 \
      "$2" | head -n 1
}

ffprobe_tag_value() {
    tag=$1
    path=$2
    value=$(ffprobe \
      -hide_banner \
      -loglevel error \
      -show_entries "format_tags=$tag" \
      -of default=noprint_wrappers=1:nokey=1 \
      "$path" | head -n 1)

    if [ -z "$value" ]; then
        value=$(ffprobe \
          -hide_banner \
          -loglevel error \
          -select_streams a:0 \
          -show_entries "stream_tags=$tag" \
          -of default=noprint_wrappers=1:nokey=1 \
          "$path" | head -n 1)
    fi

    printf '%s\n' "$value"
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

build_programs() {
    make -C "$SCRIPT_DIR" skipper silencer >/dev/null
}

generate_source() {
    ffmpeg \
      -hide_banner \
      -loglevel error \
      -y \
      -f lavfi -i "anoisesrc=color=pink:duration=$SLICE_SECONDS:amplitude=0.35" \
      -f lavfi -i "sine=frequency=220:duration=$SLICE_SECONDS" \
      -f lavfi -i "sine=frequency=330:duration=$SLICE_SECONDS" \
      -filter_complex "[0:a]highpass=f=250,lowpass=f=3400,volume=0.45[news];[1:a][2:a]amix=inputs=2:normalize=0,volume=0.18[music];[news][music]concat=n=2:v=0:a=1,aresample=$SAMPLE_RATE[a]" \
      -map "[a]" \
      -metadata date="$PROGRAMME_START" \
      -metadata title="Synthetic 6 Music Fixture" \
      -c:a aac \
      -b:a 128k \
      -ac "$CHANNELS" \
      "$SOURCE"

    assert_file_nonempty "$SOURCE"
    duration=$(ffprobe -hide_banner -loglevel error -show_entries format=duration -of default=noprint_wrappers=1:nokey=1 "$SOURCE")
    assert_duration_between "$SOURCE" "$duration" 39 41
}

run_wrapper_checks() {
    check_stdout="$WORK_DIR/skip-check.stdout"
    override_check_stdout="$WORK_DIR/skip-check-override.stdout"

    "$SCRIPT_DIR/skip_6music_news.sh" --check "$SOURCE" >"$check_stdout"
    assert_output_contains "$check_stdout" 'OK: skipper window: .*news_schedule\.ini' 'default skipper schedule'

    "$SCRIPT_DIR/skip_6music_news.sh" --check -w 58-10,28-40 "$SOURCE" >"$override_check_stdout"
    assert_output_contains "$override_check_stdout" 'OK: skipper window: 58-10,28-40' 'override skipper window'

    if "$SCRIPT_DIR/skip_6music_news.sh" --check -w >/dev/null 2>&1; then
        printf 'Error: skip_6music_news.sh accepted missing -w ranges\n' >&2
        exit 1
    fi

    silence_check_stdout="$WORK_DIR/silence-check.stdout"
    "$SCRIPT_DIR/silence_6music_news.sh" --check "$SOURCE" >"$silence_check_stdout"
    assert_output_contains "$silence_check_stdout" 'OK: silencer window: .*news_schedule\.ini' 'default silencer schedule'
}

expected_format_matches_extension() {
    format=$1
    extension=$2

    case "$extension:$format" in
        m4a:*m4a*|m4a:*mov*mp4*|mp3:*mp3*|flac:*flac*|ogg:*ogg*|wav:*wav*)
            return 0
            ;;
    esac

    return 1
}

expected_codec_matches_extension() {
    codec=$1
    extension=$2

    case "$extension:$codec" in
        m4a:aac|mp3:mp3|flac:flac|ogg:vorbis|wav:pcm_s16le)
            return 0
            ;;
    esac

    return 1
}

run_skip_file_conversion_check() {
    conversion_source=$1
    extension=${conversion_source##*.}
    base=$(basename -- "$conversion_source")
    base_no_ext=${base%.*}
    output="$WORK_DIR/${base_no_ext}_newsskip.$extension"
    log="$WORK_DIR/${base_no_ext}_newsskip.log"
    stdout="$WORK_DIR/${base_no_ext}-newsskip.stdout"

    rm -f "$output" "$log" "$stdout"

    "$SCRIPT_DIR/skip_6music_news.sh" "$conversion_source" >"$stdout"

    assert_file_nonempty "$output"
    assert_file_nonempty "$log"
    assert_output_contains "$stdout" 'Processed: [0-9]+%' 'progress percentage'
    assert_output_contains "$stdout" 'Processed: 100%' 'completion percentage'
    assert_output_contains "$stdout" 'Skipper window: .*news_schedule\.ini' 'default skipper schedule'
    assert_single_progress_line "$stdout"

    output_format=$(ffprobe_value format=format_name "$output")
    output_codec=$(ffprobe_value stream=codec_name "$output")
    output_date=$(ffprobe_tag_value date "$output")
    output_title=$(ffprobe_tag_value title "$output")
    output_sample_rate=$(ffprobe_value stream=sample_rate "$output")
    output_channels=$(ffprobe_value stream=channels "$output")

    if ! expected_format_matches_extension "$output_format" "$extension"; then
        printf 'Error: output has .%s extension but ffprobe reports format "%s"\n' "$extension" "$output_format" >&2
        exit 1
    fi

    if ! expected_codec_matches_extension "$output_codec" "$extension"; then
        printf 'Error: output has .%s extension but ffprobe reports codec "%s"\n' "$extension" "$output_codec" >&2
        exit 1
    fi

    assert_equals "$output_date" "$PROGRAMME_START" 'copied date metadata'
    assert_equals "$output_title" 'Synthetic 6 Music Fixture' 'copied title metadata'
    assert_equals "$output_sample_rate" "$SAMPLE_RATE" 'preserved sample rate'
    assert_equals "$output_channels" "$CHANNELS" 'preserved channel count'
}

run_skip_file_conversion_window_override_check() {
    output="$WORK_DIR/source_override_newsskip.m4a"
    stdout="$WORK_DIR/source-override-newsskip.stdout"

    rm -f "$output" "$stdout" "${output%.*}.log"

    "$SCRIPT_DIR/skip_6music_news.sh" --window=58-10,28-40 "$SOURCE" "$output" >"$stdout"

    assert_file_nonempty "$output"
    assert_output_contains "$stdout" 'Skipper window: 58-10,28-40' 'override skipper window'
}

generate_transcoded_fixture() {
    extension=$1
    codec_args=$2
    target="$WORK_DIR/source.$extension"

    ffmpeg \
      -hide_banner \
      -loglevel error \
      -y \
      -i "$SOURCE" \
      -vn \
      -map 0:a:0 \
      -metadata date="$PROGRAMME_START" \
      -metadata title="Synthetic 6 Music Fixture" \
      -metadata DATE="$PROGRAMME_START" \
      -metadata TITLE="Synthetic 6 Music Fixture" \
      $codec_args \
      "$target"

    assert_file_nonempty "$target"
}

run_format_conversion_checks() {
    run_skip_file_conversion_check "$SOURCE"
    run_skip_file_conversion_window_override_check

    generate_transcoded_fixture mp3 "-c:a libmp3lame -b:a 128k -ar $SAMPLE_RATE -ac $CHANNELS"
    run_skip_file_conversion_check "$WORK_DIR/source.mp3"

    generate_transcoded_fixture flac "-c:a flac -ar $SAMPLE_RATE -ac $CHANNELS"
    run_skip_file_conversion_check "$WORK_DIR/source.flac"

    generate_transcoded_fixture ogg "-c:a libvorbis -b:a 128k -ar $SAMPLE_RATE -ac $CHANNELS"
    run_skip_file_conversion_check "$WORK_DIR/source.ogg"

    generate_transcoded_fixture wav "-c:a pcm_s16le -ar $SAMPLE_RATE -ac $CHANNELS"
    run_skip_file_conversion_check "$WORK_DIR/source.wav"
}

run_silencer_slice() {
    label=$1
    offset_seconds=$2
    stream_start=$3

    input_pcm="$WORK_DIR/$label-input.pcm"
    output_pcm="$WORK_DIR/$label-output.pcm"
    log="$WORK_DIR/$label-silencer.log"

    ffmpeg \
      -hide_banner \
      -loglevel error \
      -y \
      -ss "$offset_seconds" \
      -i "$SOURCE" \
      -vn \
      -t "$SLICE_SECONDS" \
      -f s16le -ar "$SAMPLE_RATE" -ac "$CHANNELS" \
      "$input_pcm"

    "$SILENCER" -t -x -v20 -s"$SAMPLE_RATE" -T "$stream_start" -z "$PROGRAMME_OFFSET" -w "$SCRIPT_DIR/news_schedule.ini" < "$input_pcm" > "$output_pcm" 2>"$log"

    assert_file_nonempty "$input_pcm"
    assert_file_nonempty "$output_pcm"
    assert_log_contains "$log" 'Debug time:' 'stream-time debug evidence'
}

run_silencer_checks() {
    run_silencer_slice news 0 "$PROGRAMME_START"
    news_pcm="$WORK_DIR/news-output.pcm"
    news_log="$WORK_DIR/news-silencer.log"
    assert_log_contains "$news_log" 'Silenced [0-9]+ samples' 'scheduled silence evidence'
    assert_pcm_all_zero "$news_pcm"

    run_silencer_slice music "$SLICE_SECONDS" "$MUSIC_START"
    music_pcm="$WORK_DIR/music-output.pcm"
    music_log="$WORK_DIR/music-silencer.log"
    assert_log_contains "$music_log" 'Passed [0-9]+ samples' 'pass-through evidence'
    assert_pcm_has_signal "$music_pcm"
}

run_hls_mux_check() {
    hls_dir="$WORK_DIR/hls"
    playlist="$hls_dir/audio.m3u8"
    mkdir -p "$hls_dir"

    ffmpeg \
      -hide_banner \
      -loglevel error \
      -y \
      -f s16le -ar "$SAMPLE_RATE" -ac "$CHANNELS" \
      -i "$WORK_DIR/music-output.pcm" \
      -c:a aac \
      -b:a 128k \
      -f hls \
      -hls_time 4 \
      -hls_list_size 0 \
      -hls_flags independent_segments \
      -hls_segment_filename "$hls_dir/audio_%03d.ts" \
      "$playlist"

    assert_file_nonempty "$playlist"
    if ! ls "$hls_dir"/audio_*.ts >/dev/null 2>&1; then
        printf 'Error: no HLS segments were generated in %s\n' "$hls_dir" >&2
        exit 1
    fi

    hls_duration=$(playlist_duration "$playlist")
    assert_duration_between "$playlist" "$hls_duration" 16 24
}

main() {
    require_command awk
    require_command ffmpeg
    require_command ffprobe
    require_command grep
    require_command make
    require_command od

    build_programs
    generate_source
    run_wrapper_checks
    run_format_conversion_checks
    run_silencer_checks
    run_hls_mux_check

    printf 'OK: generated audio, wrapper checks, file conversion, scheduled silence/pass-through, and HLS muxing\n'
}

main "$@"
