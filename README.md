## 6 Music News Skipper

The `.sh` files in this repository are convenience wrappers for BBC 6 Music
news/talk removal workflows. They use `ffmpeg`, `ffprobe`, `ffplay`, `curl`,
`make`, and the local `skipper` or `silencer` executables. Each script builds
the executable it needs if it is missing.

Run any script from a shell:

```
./script-name.sh --help
./script-name.sh --check
```

`--check` is a smoke test. It verifies the required commands are installed,
builds the relevant executable if needed, decodes a short amount of audio, and
checks that the pipeline can run before starting a longer conversion or live
stream.

### Recorded programme scripts

`skip_6music_news.sh` processes a recorded `.m4a` file through `skipper` and
writes a new AAC `.m4a` file with detected talk/news skipped. It reads the
programme start time from the input file's `date` metadata using `ffprobe`, so
the recording must include that tag.

```
./skip_6music_news.sh --check input.m4a
./skip_6music_news.sh input.m4a output_skipped_news.m4a
```

If no input is provided, the script uses the hard-coded Gilles Peterson iPlayer
recording path in the script. If no output is provided, it writes beside this
repository using the input filename plus `_skipped_news.m4a`. A matching `.log`
file is written next to the output.

`silence_6music_news.sh` has the same recorded-file interface, but it runs the
`silencer` executable instead of `skipper`. Use it when you want the newer
stream-time aware silencing path.

```
./silence_6music_news.sh --check input.m4a
./silence_6music_news.sh input.m4a output_silenced_talk.m4a
```

### Live playback scripts

`play_6music_silencer.sh` plays the live BBC 6 Music HLS stream through
`silencer`, removes detected talk, and sends the resulting raw PCM audio to
`ffplay` for local listening.

```
./play_6music_silencer.sh --check
./play_6music_silencer.sh
URL=https://example.com/stream.m3u8 ./play_6music_silencer.sh
```

The script reads `#EXT-X-PROGRAM-DATE-TIME` from the HLS playlist and uses the
current Europe/London UTC offset so the silencer can align scheduled news
windows with the station clock.

`radio6music_noNews_fip.sh` plays live BBC 6 Music with detected talk removed
and mixes in FIP during the silent gaps. FIP is sidechain-ducked by the BBC
audio, so it fades down when 6 Music returns.

```
./radio6music_noNews_fip.sh --check
./radio6music_noNews_fip.sh
BBC_URL=https://example.com/bbc.m3u8 FIP_URL=https://example.com/fip.m3u8 ./radio6music_noNews_fip.sh
```

For an Icecast output, run it with `-AWS` and set the Icecast URL if the default
does not match your server:

```
ICECAST_SOURCE_PASSWORD=password ./radio6music_noNews_fip.sh -AWS
AWS_ICECAST_URL=icecast://source:password@host:8000/mount.mp3 ./radio6music_noNews_fip.sh -AWS
```

Optional environment variables include `FIP_VOLUME`, `DUCK_THRESHOLD`,
`DUCK_RATIO`, `FIP_FADE_OUT_MS`, `FIP_FADE_IN_MS`, `AWS_AUDIO_BITRATE`, and
`AWS_RESTART_DELAY_SECONDS`.

The Icecast/Alexa MP3 output is stereo. In AWS mode, the script uses more
patient FFmpeg reconnect settings and restarts the stream pipeline after a
source or Icecast connection failure. The final mix is timestamp-smoothed before
MP3 encoding to reduce short player underruns during long streams.
The encoder reads the silencer output at realtime speed so Icecast receives
steady audio rather than one-second PCM bursts.
`AWS_RESTART_DELAY_SECONDS` controls the pause between restart attempts.

### AWS deployment

This branch includes a Docker Compose deployment for EC2. It runs the MP3
stream pipeline, an HLS stream pipeline, Icecast, and Caddy HTTPS proxy. The
Alexa-ready MP3 stream URL is:

```
https://PUBLIC_HOST/the-radio.mp3
```

The HLS test stream URL is:

```
https://PUBLIC_HOST/hls/radio6music_noNews_fip_plex.m3u8
```

For first AWS testing, `PUBLIC_HOST` can be an `sslip.io` hostname such as
`203.0.113.10.sslip.io`. See `docs/aws-deploy.md` for the full runbook and
`alexa-skill/README.md` for the minimal Alexa skill scaffold.

`radio6music_noNews_fip_plex.sh` creates a rolling HLS playlist and `.ts`
segments for Plex, Caddy, or another player that can read HLS files. Keep it
running while listening; it continuously refreshes the playlist.

```
./radio6music_noNews_fip_plex.sh --check
./radio6music_noNews_fip_plex.sh
OUT_DIR=/path/visible/to/plex ./radio6music_noNews_fip_plex.sh
```

By default it writes to `plex_radio6music_noNews_fip/` in this repository. The
main playlist is `radio6music_noNews_fip_plex.m3u8`, and logs are written to
`radio6music_noNews_fip_plex.log`. Useful tuning variables include `OUT_DIR`,
`HLS_AUDIO_BITRATE`, `HLS_TIME`, `HLS_LIST_SIZE`, `HLS_RESTART_DELAY_SECONDS`,
`BBC_URL`, `FIP_URL`, and the FIP ducking controls listed above.

## About Skipper

This project is based on David Bryant's original **Skipper** work, a selective
audio detection and filter tool.

Copyright (c) 2024 David Bryant.

All Rights Reserved.

Distributed under the [BSD Software License](https://github.com/dbry/skipper/blob/main/LICENSE).

**Skipper** is a simple machine-learning-trained audio filter that can
differentiate between musical material and talking in audio streams
and, optionally, filter out (i.e., skip) one or the other.

The original project was developed for listening to and archiving FM radio and
Internet music programme streams. These are useful for discovering new music,
learning about a local music scene, and listening to interviews with artists and
others in the music community. The difficulty is that replayed programmes can
contain repeated or outdated dialogue, such as upcoming concert listings or
finished pledge drives, and sometimes the dialogue is simply not wanted.

By default, **Skipper** acts as a filter, consuming raw PCM audio, stereo or
mono 16-bit, from `stdin` and writing it unchanged, except always stereo, to
`stdout`. It detects music/talk transitions and reports those timestamps to
`stderr`.

Specifying `-t` skips over detected talk and passes only music, with crossfades
to smooth transitions. Conversely, `-m` skips over detected music and passes
only the talking portions, which is useful for checking the detection quality.

## Caveats

It is not possible to distinguish music and talk with 100% accuracy. Detection
becomes difficult when a DJ talks over music, when music includes speech-like
singing or spoken samples, or when a genre has a temporal acoustic profile that
resembles speech. The command-line options allow threshold adjustment when too
much talk is kept or too much music is skipped.

## Building

The Makefile builds the programs on Linux and similar setups.

The `skipper` executable is the core original filter. The `silencer` executable
is used by the BBC 6 Music helper scripts in this branch. The `tensor-gen` and
`bin2c` executables are used, along with the `-a` option of `skipper`, for
generating tensor files from training audio data.

## Skipper usage

There are many ways to use **Skipper**, but a common approach is to use
[FFmpeg](https://www.ffmpeg.org/) as the source because it handles many formats
and works well with pipes. The output of `ffmpeg` can be piped directly to
`skipper`, then to an encoder such as [lame](https://lame.sourceforge.io/):

```
ffmpeg -i sourcefile.ext -f s16le - | ./skipper -t | lame -r - music-only.mp3
```

The output of `skipper` can also be piped directly to
[FFplay](https://www.ffmpeg.org/) for immediate playback. In this case, `-k`
adds keep-alive crossfades during long skips so playback does not underrun.

```
ffmpeg -i sourcefile.ext -f s16le - | ./skipper -tk | ffplay - -f s16le -ch_layout stereo
```

Currently, **Skipper** is available as a command-line filter.

## Help

```
 SKIPPER  Selective Audio Detection and Filter  Version 0.1
 Copyright (c) 2024 David Bryant. All Rights Reserved.

 Usage:     SKIPPER [-options] < SourceAudio.pcm > StereoOutput.pcm

 Operation: scan source audio (`stdin`) using tensor discrimination to filter
            output (`stdout`), skipping either music (-m) or talk (-t); or
            output raw scan analytics for use with TENSOR-GEN util (-a)

 Options:  -a <file.bin>    = output analysis results to specified file
           -c<n>            = override default channel count of 2
           -d <file.tensor> = specify alternate discrimination tensor file
           -k               = keep-alive crossfading for long skips
           -l<n>            = left output override (for debug, n = 1-4:
                            = 1=mono, 2=filtered, 3=level, 4=tensor)
           -m[<n>]          = skip over music, with optional threshold offset
                            = (raise or lower music threshold +/- 99 points)
           -n               = no audio output (skip everything)
           -p               = pass all audio (no skipping, default)
           -q               = no messaging except errors
           -r<n>            = right output override (for debug, n = 1-4:
                            = 1=mono, 2=filtered, 3=level, 4=tensor)
           -s<n>            = override default sample rate of 44.1 kHz
           -t[<n>]          = skip over talk, with optional threshold offset
                            = (raise or lower talk threshold +/- 99 points)
           -v[<n>]          = set verbosity + [rate in seconds]

 Web:      Visit www.github.com/dbry/skipper for latest version and info

```
