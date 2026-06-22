FROM debian:bookworm-slim

RUN apt-get update \
    && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        ca-certificates \
        icecast2 \
    && rm -rf /var/lib/apt/lists/*

COPY docker/icecast/icecast.xml.template /etc/icecast2/icecast.xml.template
COPY docker/icecast/entrypoint.sh /usr/local/bin/icecast-entrypoint

RUN chmod +x /usr/local/bin/icecast-entrypoint \
    && mkdir -p /var/log/icecast2 \
    && chown -R icecast2 /etc/icecast2 /var/log/icecast2

EXPOSE 8000

USER icecast2

ENTRYPOINT ["icecast-entrypoint"]
