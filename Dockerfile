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

RUN make silencer

FROM debian:bookworm-slim

RUN apt-get update \
    && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        ca-certificates \
        curl \
        ffmpeg \
        make \
        tzdata \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY Makefile ./
COPY *.c *.h ./
COPY radio6music_noNews_fip.sh ./
COPY --from=build /app/silencer ./silencer

RUN chmod +x ./radio6music_noNews_fip.sh

CMD ["./radio6music_noNews_fip.sh", "-AWS"]
