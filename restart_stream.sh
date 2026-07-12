#!/bin/sh

cd -- "$(dirname -- "$0")" || exit 1
exec docker compose restart hls-streamer
