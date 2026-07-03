FROM debian:bookworm-slim AS build

RUN apt-get update \
    && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        build-essential \
        ca-certificates \
        make \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY Makefile ./
COPY *.c *.h ./

RUN make silencer hls_schedule_state

FROM debian:bookworm-slim

RUN apt-get update \
    && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        ca-certificates \
        curl \
        ffmpeg \
        tzdata \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY radio6music_noNews_hls.sh ./
COPY news_schedule.ini ./
COPY --from=build /app/silencer ./silencer
COPY --from=build /app/hls_schedule_state ./hls_schedule_state

RUN chmod +x ./radio6music_noNews_hls.sh

CMD ["./radio6music_noNews_hls.sh"]
