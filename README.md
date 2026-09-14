## 6 Music News Skipper

This repo builds two PCM filters and a small set of BBC 6 Music helper
scripts.

- `skipper` removes selected sections from the stream, so output duration can
  be shorter than input duration based on [Selective Audio Detection and Filter Copyright (c) 2024 David Bryant.](https://github.com/dbry/skipper)
  
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
- `curl` for the live HLS script

Build the C tools:

```sh
make
```

The model-generation utilities are not part of the runtime build. Build them
only when regenerating the embedded classifier data:

```sh
make tools
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

The recorded-file wrapper and live HLS service default to
`NEWS_SCHEDULE=${repo}/news_schedule.ini`. Override the recorded-file wrapper
with either a schedule file or legacy minute ranges:

```sh
NEWS_SCHEDULE=/path/to/schedule.ini ./skip_6music_news.sh input.m4a
./skip_6music_news.sh -w news_schedule.ini input.m4a
./skip_6music_news.sh -w 58-10,28-40 input.m4a
```

The C programs receive the schedule through `-w`. They also need stream time
with `-T` and UTC offset with `-z` for schedule-aware filtering.

For the live HLS service, set `NEWS_SCHEDULE` to another schedule file. The
legacy minute-range CLI is intentionally limited to recorded-file processing.

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

Open `https://PUBLIC_HOST/`. The page provides:

- the news-skipped live stream with BBC track metadata
- BBC Sounds programme processing with progress
- searchable BBC Radio 6 Music programme selection from the hourly get_iplayer cache
- playback, download, and removal of completed programmes
- current-track details during live and processed playback
- a browser-local favourites list, with Google searches from every track title

Playback does not start automatically and only one player can play at a time.
On browsers that support Media Session, including current iPhone Safari, the
active live or processed track is also published to the system Now Playing UI
with artist, show/presenter context, artwork, playback state, and offline seek
position.
Completed files persist in the `skipper_downloads` volume. The HLS stream is
also available at `https://PUBLIC_HOST/hls/radio6music_noNews.m3u8`.
Removing a processed programme deletes its processed media, logs, artwork, and
track list, plus any exact PID-named original media and matching sidecars. Other
processed versions and unrelated files are left in place.

Programme downloads also retain get_iplayer's track list. Skipper records
absolute input, output, and discarded-sample checkpoints and uses them to move
each original BBC track start onto the shortened audio timeline without changing
the track duration. The `.tracks.txt` and `.log` sidecars must remain beside the
processed media file for offline track details. Offline favourites also retain
the show and presenter from embedded M4A metadata, falling back to the track-list
header. Favourites use a Last.fm-compatible track shape in browser storage; no
Last.fm account or API key is required yet.

Set `BBC_NOW_PLAYING_URL` to override the BBC metadata endpoint or
`BBC_NOW_PLAYING_DELAY_SECONDS` to adjust the default 18-second display delay.

For host-only use without Docker:

```sh
python3 skip_news_web.py
```

The recorded-file wrapper supports `--profile`, which times decode, filter,
and encode stages separately:

```sh
./skip_6music_news.sh --profile input.m4a output.m4a
```

## Live Scripts

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
- `NEWS_SCHEDULE`
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
- `radio6music_noNews_hls.sh`: live HLS generation with scheduled news
  replacement.
- `restart_stream.sh`: rebuilds and force-recreates the Compose stack.
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
