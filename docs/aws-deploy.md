# AWS EC2 Docker Deployment

This deployment runs two containers on one EC2 instance:

- `hls-streamer`: writes a rolling AAC/HLS playlist and segments
- `caddy`: exposes `https://PUBLIC_HOST/hls/radio6music_noNews.m3u8`
  on ports `80` and `443`

Alexa needs an HTTPS stream URL on port `443` with a trusted certificate. For
first testing, use `sslip.io` DNS:

```
https://EC2_PUBLIC_IP.sslip.io/hls/radio6music_noNews.m3u8
```

Replace `EC2_PUBLIC_IP` with the instance public IPv4 address.

## 1. Create The EC2 Instance

Recommended starting point:

- AMI: Ubuntu Server 24.04 LTS
- Instance type: `t3.small`
- Storage: 16 GB gp3
- Region: whichever is closest to you, for example `eu-west-2`

Security group inbound rules:

| Type | Port | Source |
| --- | --- | --- |
| SSH | 22 | Your IP only |
| HTTP | 80 | `0.0.0.0/0`, `::/0` |
| HTTPS | 443 | `0.0.0.0/0`, `::/0` |

Caddy is the public entrypoint; no other application ports need to be opened.

## 2. Install Docker

SSH into the instance, then run:

```
sudo apt update
sudo apt install -y ca-certificates curl git gnupg
sudo install -m 0755 -d /etc/apt/keyrings
curl -fsSL https://download.docker.com/linux/ubuntu/gpg | sudo gpg --dearmor -o /etc/apt/keyrings/docker.gpg
sudo chmod a+r /etc/apt/keyrings/docker.gpg

echo \
  "deb [arch=$(dpkg --print-architecture) signed-by=/etc/apt/keyrings/docker.gpg] https://download.docker.com/linux/ubuntu \
  $(. /etc/os-release && echo "$VERSION_CODENAME") stable" | \
  sudo tee /etc/apt/sources.list.d/docker.list > /dev/null

sudo apt update
sudo apt install -y docker-ce docker-ce-cli containerd.io docker-buildx-plugin docker-compose-plugin
sudo usermod -aG docker "$USER"
newgrp docker
```

Check:

```
docker version
docker compose version
```

## 3. Clone And Configure

Clone the repo:

```
git clone -b NewsSchedule git@github.com:Danjeli303/6musicNoNews.git skipper
cd skipper
```

If the EC2 instance does not have GitHub SSH access yet, use the HTTPS clone URL
or add the instance SSH public key as a GitHub deploy key.

Create the environment file:

```
cp .env.example .env
PUBLIC_IP=$(curl -fsS https://checkip.amazonaws.com | tr -d '\n')
sed -i "s/203.0.113.10/${PUBLIC_IP}/" .env
```

Edit `.env` if you need to change `PUBLIC_HOST` or tune the HLS stream:

```
nano .env
```

## 4. Start The Stream

Validate Compose config:

```
docker compose config
```

Build and start:

```
docker compose up -d --build
```

Watch startup:

```
docker compose logs -f hls-streamer caddy
```

The public stream URL is:

```
https://PUBLIC_HOST/hls/radio6music_noNews.m3u8
```

For example:

```
https://203.0.113.10.sslip.io/hls/radio6music_noNews.m3u8
```

The HLS stream uses AAC segments with `HLS_AUDIO_BITRATE=128k`,
`HLS_TIME=6`, and `HLS_LIST_SIZE=20` by default. It is served directly by Caddy
from the shared `hls_data` volume. Set `HLS_RESTART_DELAY_SECONDS` in `.env` if
you want a longer or shorter pause between restart attempts after an upstream
stream failure; the default is `1` second so transient failures recover quickly.
Restarts keep the last good playlist and segments in place for continuity. Set
`HLS_CLEAN_START=1` only when you intentionally want the container to discard
the existing HLS window at startup.

## 5. Smoke Tests

From your laptop or the EC2 instance:

```
curl -I https://PUBLIC_HOST/hls/radio6music_noNews.m3u8
```

For audio validation, use the `ffprobe` installed inside the hls-streamer image:

```
docker compose exec hls-streamer ffprobe https://PUBLIC_HOST/hls/radio6music_noNews.m3u8
```

You can also paste the URL into VLC or a browser.

If Caddy has not finished obtaining the certificate yet, wait a minute and
check:

```
docker compose logs caddy
```

## 6. Alexa

Use the stream URL as the Alexa Lambda `STREAM_URL`:

```
STREAM_URL=https://PUBLIC_HOST/hls/radio6music_noNews.m3u8
```

See `alexa-skill/README.md` for the minimal skill setup.

## Operations

Restart everything:

```
docker compose restart
```

Rebuild after pulling repo changes:

```
git pull
docker compose up -d --build
```

Stop the stack:

```
docker compose down
```

Stop and remove Caddy certificate/config volumes:

```
docker compose down -v
```

Cost cleanup:

1. Stop or terminate the EC2 instance.
2. Delete unattached EBS volumes if any were left behind.
3. Release any Elastic IP if you created one.
4. Remove old snapshots or AMIs if you made them.

## Troubleshooting

- `PUBLIC_HOST` must point at the EC2 public IP. With `sslip.io`, use the form
  `203.0.113.10.sslip.io`.
- Ports `80` and `443` must be open to the internet for Caddy certificate
  issuance and Alexa playback.
- If the stream is silent, inspect `docker compose logs hls-streamer`.
- If HTTPS fails, inspect `docker compose logs caddy`.
