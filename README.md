## 6 Music News Skipper

This repo builds two PCM filters and a small set of BBC 6 Music helper
scripts.

- `skipper` removes selected sections from the stream, so output duration can
  be shorter than input duration.
- `silencer` keeps the original duration and replaces selected sections with
  silence.
- The wrapper scripts use `ffmpeg`/`ffprobe` to decode files or live streams,
  pass PCM through the local filter, then encode or play the result.

The branch is configured for scheduled BBC 6 Music news removal. The default
schedule is [news_schedule.ini](news_schedule.ini), with a 2 minute window
before each listed bulletin time and a 5 minute window after it.

## Build

Requirements for local development:

- C compiler with `make`
- `ffmpeg` and `ffprobe`
- `get_iplayer` for BBC Sounds programme downloads
- `curl` for live HLS scripts
- `ffplay` only for local live playback

Build the C tools:

```sh
make
```

Production builds use `OPTFLAGS ?= -Ofast -flto`. Override when needed:

```sh
make -B OPTFLAGS='-Ofast' skipper silencer
```

Generated binaries and local audio outputs are ignored by git. Remove build
artifacts with:

```sh
make clean
```

## Schedule Input

Scripts default to `NEWS_SCHEDULE=${repo}/news_schedule.ini`. Override with
either a schedule file or legacy minute ranges:

```sh
NEWS_SCHEDULE=/path/to/schedule.ini ./skip_6music_news.sh input.m4a
./skip_6music_news.sh -w news_schedule.ini input.m4a
./skip_6music_news.sh -w 58-10,28-40 input.m4a
```

The C programs receive the schedule through `-w`. They also need stream time
with `-T` and UTC offset with `-z` for schedule-aware filtering.

## Recorded Files

Use `skip_6music_news.sh` for offline recordings where you want news removed:

```sh
./skip_6music_news.sh --check input.m4a
./skip_6music_news.sh input.m4a
./skip_6music_news.sh input.m4a output_newsskip.m4a
```

The input must contain a `date` metadata tag so the script can align the
recording with the schedule. With one input argument, the output is written
beside the input using `_newsskip` before the extension.

Use `silence_6music_news.sh` when you want a same-duration file with news
silenced:

```sh
./silence_6music_news.sh --check input.m4a
./silence_6music_news.sh input.m4a output_silenced_talk.m4a
```

### Download from BBC Sounds

`get_iplayer_skip_news.sh` accepts an individual BBC Sounds programme link,
downloads the radio programme to an isolated temporary directory, runs
`skip_6music_news.sh`, and saves only the news-skipped result:

```sh
./get_iplayer_skip_news.sh https://www.bbc.co.uk/sounds/play/m0030yw7
./get_iplayer_skip_news.sh https://www.bbc.co.uk/sounds/play/m0030yw7 /path/to/output
```

The default output directory is `downloads/`. Install `get_iplayer` by following
its [official installation guide](https://github.com/get-iplayer/get_iplayer/wiki/installation)
when running the script directly on the host. The Docker setup below already
contains it.

### Web interface with live progress

Build and start the complete HLS and news-skipper stack with Docker Compose:

```sh
docker compose up -d --build
```

Open `https://PUBLIC_HOST/`, paste a BBC Sounds `/sounds/play/` link, and start
the job. Caddy serves the page on the same hostname and HTTPS ports as the HLS
stream, which remains at
`https://PUBLIC_HOST/hls/radio6music_noNews.m3u8`. The page includes a bundled
[Video.js 8.24.1](https://github.com/videojs/video.js) player for this live HLS
stream, reports download and news-removal progress, then displays a download
button for the completed audio file. The activity section shows queued and
running processes with live status, alongside previously processed programmes
with their embedded title, artist, album, description and cover art. Completed
programmes can be played with Video.js, downloaded again, or removed from the
server. Playback never starts automatically and starting any player stops all
other live or recorded audio. Results persist in the `skipper_downloads` Docker
volume and reappear after a container restart. Removing an item deletes its
audio, processing log and extracted artwork. Both the HLS worker and web worker
run from the same `six-music-skipper:local` image built by Compose.

For host-only use without Docker:

```sh
python3 skip_news_web.py
```

Both wrappers support `--profile`, which times decode, filter, and encode
stages separately:

```sh
./skip_6music_news.sh --profile input.m4a output.m4a
./silence_6music_news.sh --profile input.m4a output.m4a
```

## Live Scripts

Local playback:

```sh
./play_6music_silencer.sh --check
./play_6music_silencer.sh
```

HLS output for AWS/Caddy:

```sh
./radio6music_noNews_hls.sh --check
./radio6music_noNews_hls.sh
```

The HLS script writes a rolling playlist to `hls_radio6music_noNews/` by
default. Useful environment variables:

- `OUT_DIR`
- `HLS_AUDIO_BITRATE`
- `HLS_AAC_CODER`
- `HLS_TIME`
- `HLS_LIST_SIZE`
- `HLS_CLEAN_START`
- `BBC_URL`
- `FIP_URL`

AWS deployment notes are in [docs/aws-deploy.md](docs/aws-deploy.md). The Alexa
skill scaffold is in [alexa-skill/README.md](alexa-skill/README.md).

## Tests

```sh
make test
make audio-test
make sample-recording-test
```

- `make test` runs C unit tests.
- `make web-test` runs the BBC Sounds wrapper and web-service unit tests without
  downloading a real programme.
- `make audio-test` creates temporary synthetic audio, checks wrappers, verifies
  scheduled silence/pass-through, checks format preservation, and validates HLS
  packaging. It does not use live streams.
- `make sample-recording-test` is opt-in for a local real recording. Set
  `RUN_LOCAL_SAMPLE_TEST=1` and optionally `SAMPLE_RECORDING=/path/to/file.m4a`.

## Code Map

- `skipper.c`: original skip/remove filter with schedule-aware fast passthrough
  outside news windows.
- `silencer.c`: same classifier path, but writes silence instead of shortening
  the stream; also bypasses analysis outside scheduled windows while preserving
  timing delay.
- `skipper_time.c` / `skipper_time.h`: ISO-8601 parsing, UTC offset parsing,
  INI schedule parsing, and active-window checks.
- `skipper_tensor.c` / `skipper_tensor.h`: embedded tensor loading helpers.
- `4d-tensor.h`: embedded classifier tensor.
- `audio_validation_tests.sh`: end-to-end wrapper/audio validation.
- `get_iplayer_skip_news.sh`: temporary BBC Sounds download and recorded-file
  news-removal wrapper.
- `skip_news_web.py` and `web/`: local web service with progress and downloads.
- `skipper_tests.c` and `silencer_tests.c`: C unit tests.
- `Dockerfile`, `docker-compose.yml`, and `docker/caddy/Caddyfile`: AWS HLS
  deployment packaging.

## C Profiling

Set these environment variables when running the C binaries directly:

```sh
SKIPPER_PROFILE=1 ./skipper ...
SILENCER_PROFILE=1 ./silencer ...
```

The profile summary is written to `stderr` and includes read, prepare,
process-loop, `analyze_window`, buffer-shift, fast-passthrough, and flush
timings.
