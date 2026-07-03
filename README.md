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
default. The FIP stream is opened and mixed only during the configured news
schedule, using the same `NEWS_SCHEDULE`/`-w` window as the BBC silencer.
Useful environment variables:

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
