## 6 Music News Skipper

This repo builds a recorded-audio filter, a live news detector, and a small set
of BBC Radio 6 Music helper scripts.

- `skipper` removes selected sections from the stream, so output duration can
  be shorter than input duration based on [Selective Audio Detection and Filter Copyright (c) 2024 David Bryant.](https://github.com/dbry/skipper)
  
- `news_identifier` reads live PCM, identifies scheduled news, and emits only
  `news_on` and `news_off` events. It never outputs audio.
- For the live stream, FFmpeg keeps the original BBC audio and crossfades
  between BBC Radio 6 Music and FIP in response to those events.
- The recorded-file wrappers use `ffmpeg`/`ffprobe` to decode audio, pass PCM
  through `skipper`, then encode the shortened result.

The branch is configured for scheduled BBC 6 Music news removal. The default
schedule is [news_schedule.ini](news_schedule.ini), with a 2 minute window
before each listed bulletin time and a 5 minute window after it.

## Build

Requirements for local development:

- C compiler with `make`
- `ffmpeg` and `ffprobe`
- ZeroMQ development files and `pkg-config` (for `news_mixer_control`)
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
make -B OPTFLAGS='-Ofast' skipper news_identifier news_mixer_control
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

- the news-skipped live stream with BBC metadata during normal playback
- FIP programme, track, and artwork metadata while FIP replaces scheduled news
- a small FIP switch on the live player for manually fading between stations
- BBC Sounds programme processing with progress
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
`FIP_NOW_PLAYING_URL` to override Radio France's public live-metadata endpoint.
Set `BBC_NOW_PLAYING_DELAY_SECONDS` to adjust the default 18-second display
delay shared by both sources so the information follows the HLS playback
latency.

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
- `FIP_VOLUME`
- `FIP_NOW_PLAYING_URL`
- `BBC_FADE_OUT_MS`
- `BBC_FADE_IN_MS`
- `MIXER_CONTROL_ENDPOINT`
- `NEWS_STATUS_FILE`
- `NEWS_CONTROL_PIPE`

The live script sends a copy of decoded BBC PCM to `news_identifier`. FFmpeg
delays only the untouched BBC branch by the classifier look-ahead; FIP remains
live and current. The identifier emits `NEWS_EVENT` lines on standard error.
`news_mixer_control` applies those events to named FFmpeg volume filters over
ZeroMQ: the latest `news_on` event selects FIP and the latest `news_off` event
selects BBC, even when an event is repeated. There are no separate schedule
events or delayed station changes. The controller atomically writes `news-status.json`,
including when a fade is in progress. This tells the web service when to disable
the station switch and when to show FIP programme, track, and artwork metadata.
The live-player FIP switch writes manual `news_on` and `news_off`
events to the same control FIFO, so it uses the same fades as automatic news
replacement. After each use it is disabled for 30 seconds while the stream
transition reaches listeners.

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
  scheduled identifier events and its empty audio output, checks format
  preservation, and validates HLS packaging. It does not use live streams.
- `make sample-recording-test` is opt-in for a local real recording. Set
  `RUN_LOCAL_SAMPLE_TEST=1` and optionally `SAMPLE_RECORDING=/path/to/file.m4a`.

## Code Map

- `skipper.c`: original skip/remove filter with schedule-aware fast passthrough
  outside news windows.
- `NewsIdentifier.c`: event-only scheduled-news classifier. It consumes PCM and
  emits schedule/news transitions without writing audio.
- `news_mixer_control.c`: receives identifier events and crossfades the named
  BBC/FIP FFmpeg volume filters while publishing `news-status.json`.
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
- `skipper_tests.c`, `news_identifier_tests.c`, and
  `news_mixer_control_tests.c`: C unit tests.
- `Dockerfile`, `docker-compose.yml`, and `docker/caddy/Caddyfile`: AWS HLS
  deployment packaging.

## C Profiling

Set these environment variables when running the C binaries directly:

```sh
SKIPPER_PROFILE=1 ./skipper ...
NEWS_IDENTIFIER_PROFILE=1 ./news_identifier ...
```

The profile summary is written to `stderr` and includes read, prepare,
process-loop, `analyze_window`, buffer-shift, fast-passthrough, and flush
timings.
