#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
GET_IPLAYER_BIN="${GET_IPLAYER_BIN:-get_iplayer}"
SKIP_NEWS_SCRIPT="${SKIP_NEWS_SCRIPT:-$SCRIPT_DIR/skip_6music_news.sh}"

usage() {
    printf 'Usage: %s BBC_SOUNDS_URL [output-directory]\n' "$0"
    printf 'Example: %s https://www.bbc.co.uk/sounds/play/m0030yw7 ./downloads\n' "$0"
}

require_command() {
    if ! command -v "$1" >/dev/null 2>&1; then
        printf 'Error: required command not found: %s\n' "$1" >&2
        exit 1
    fi
}

if [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then
    usage >&2
    exit 1
fi

case "$1" in
    -h|--help)
        usage
        exit 0
        ;;
esac

SOUNDS_URL=$1
OUTPUT_DIR=${2:-"$SCRIPT_DIR/downloads"}

require_command grep
require_command sed

if ! printf '%s\n' "$SOUNDS_URL" | grep -Eq '^https?://(www\.)?bbc\.co\.uk/sounds/play/[a-z0-9]{8}([/?#].*)?$'; then
    printf 'Error: enter a BBC Sounds programme URL such as https://www.bbc.co.uk/sounds/play/m0030yw7\n' >&2
    exit 1
fi

PID=$(printf '%s\n' "$SOUNDS_URL" | sed -E 's#^https?://(www\.)?bbc\.co\.uk/sounds/play/([a-z0-9]{8}).*$#\2#')

require_command "$GET_IPLAYER_BIN"

if [ ! -x "$SKIP_NEWS_SCRIPT" ]; then
    printf 'Error: news-skipping script is not executable: %s\n' "$SKIP_NEWS_SCRIPT" >&2
    exit 1
fi

mkdir -p "$OUTPUT_DIR"
OUTPUT_DIR=$(CDPATH= cd -- "$OUTPUT_DIR" && pwd)
WORK_DIR=$(mktemp -d "${TMPDIR:-/tmp}/get-iplayer-news-skip.XXXXXX")
PROFILE_DIR="$WORK_DIR/profile"
mkdir -p "$PROFILE_DIR"

OUTPUT_FILE=
COMPLETED=0
cleanup() {
    if [ "$COMPLETED" -ne 1 ] && [ -n "$OUTPUT_FILE" ]; then
        rm -f "$OUTPUT_FILE"
    fi
    rm -rf "$WORK_DIR"
}
trap cleanup EXIT HUP INT TERM

printf 'STAGE=download\n'
printf 'Downloading BBC Sounds programme %s...\n' "$PID"
"$GET_IPLAYER_BIN" \
    --pid="$PID" \
    --type=radio \
    --force \
    --radio-quality=high,std,med,low \
    --log-progress \
    --profile-dir="$PROFILE_DIR" \
    --output="$WORK_DIR" \
    --file-prefix="$PID"

DOWNLOADED_FILE=
for candidate in "$WORK_DIR/$PID".*; do
    if [ ! -f "$candidate" ]; then
        continue
    fi

    extension=$(printf '%s\n' "${candidate##*.}" | tr '[:upper:]' '[:lower:]')
    case "$extension" in
        m4a|m4b|mp4|mp3|flac|ogg|oga|opus|wav)
            DOWNLOADED_FILE=$candidate
            break
            ;;
    esac
done

if [ -z "$DOWNLOADED_FILE" ]; then
    printf 'Error: get_iplayer completed without creating a supported audio file.\n' >&2
    exit 1
fi

EXTENSION=$(printf '%s\n' "${DOWNLOADED_FILE##*.}" | tr '[:upper:]' '[:lower:]')
OUTPUT_FILE="$OUTPUT_DIR/${PID}_newsskip.$EXTENSION"
output_number=2
while [ -e "$OUTPUT_FILE" ]; do
    OUTPUT_FILE="$OUTPUT_DIR/${PID}_newsskip_${output_number}.$EXTENSION"
    output_number=$((output_number + 1))
done

printf 'STAGE=process\n'
printf 'Removing scheduled news from %s...\n' "$(basename -- "$DOWNLOADED_FILE")"
"$SKIP_NEWS_SCRIPT" "$DOWNLOADED_FILE" "$OUTPUT_FILE"

if [ ! -s "$OUTPUT_FILE" ]; then
    printf 'Error: news-skipping completed without creating an output file.\n' >&2
    exit 1
fi

COMPLETED=1
printf 'STAGE=complete\n'
printf 'OUTPUT_FILE=%s\n' "$OUTPUT_FILE"
printf 'Done: %s\n' "$OUTPUT_FILE"
