FROM debian:bookworm-slim AS build

RUN apt-get update \
    && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        build-essential \
        ca-certificates \
        libzmq3-dev \
        make \
        pkg-config \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY Makefile ./
COPY *.c *.h ./

RUN make news_identifier news_mixer_control skipper

FROM debian:bookworm-slim AS audio-runtime

RUN apt-get update \
    && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        ca-certificates \
        curl \
        ffmpeg \
        libzmq5 \
        tzdata \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY radio6music_noNews_hls.sh ./
COPY skip_6music_news.sh ./
COPY news_schedule.ini ./
COPY --from=build /app/news_identifier ./news_identifier
COPY --from=build /app/news_mixer_control ./news_mixer_control
COPY --from=build /app/skipper ./skipper

FROM audio-runtime AS skipper-runtime

ARG GET_IPLAYER_VERSION=v3.36

RUN apt-get update \
    && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        atomicparsley \
        libcgi-pm-perl \
        liblwp-protocol-https-perl \
        libmojolicious-perl \
        libwww-perl \
        libxml-libxml-perl \
        perl \
        python3 \
    && curl -fsSL \
        "https://raw.githubusercontent.com/get-iplayer/get_iplayer/${GET_IPLAYER_VERSION}/get_iplayer" \
        -o /usr/local/bin/get_iplayer \
    && chmod 0755 /usr/local/bin/get_iplayer \
    && get_iplayer -V \
    && rm -rf /var/lib/apt/lists/*

COPY get_iplayer_skip_news.sh skip_news_web.py ./
COPY web ./web

RUN chmod +x \
        ./get_iplayer_skip_news.sh \
        ./radio6music_noNews_hls.sh \
        ./skip_6music_news.sh \
        ./skip_news_web.py \
    && mkdir -p /srv/downloads

EXPOSE 8080

CMD ["./radio6music_noNews_hls.sh"]
