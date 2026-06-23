# AWS EC2 Docker Deployment

This deployment runs three containers on one EC2 instance:

- `streamer`: runs `./radio6music_noNews_fip.sh -AWS`
- `icecast`: receives the MP3 stream privately on the Docker network
- `caddy`: exposes `https://PUBLIC_HOST/the-radio.mp3` on ports `80` and `443`

Alexa needs an HTTPS stream URL on port `443` with a trusted certificate. For
first testing, use `sslip.io` DNS:

```
https://EC2_PUBLIC_IP.sslip.io/the-radio.mp3
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

Do not open Icecast port `8000`; Caddy is the public entrypoint.

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
git clone -b AWS git@github.com:Danjeli303/6musicNoNews.git skipper
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

Edit `.env` and set unique passwords:

```
nano .env
```

Use simple password characters for `ICECAST_SOURCE_PASSWORD`, such as letters,
numbers, dashes, and underscores. The streamer embeds this password in an
Icecast URL.

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
docker compose logs -f streamer
```

The public stream URL is:

```
https://PUBLIC_HOST/the-radio.mp3
```

For example:

```
https://203.0.113.10.sslip.io/the-radio.mp3
```

The deployed MP3 stream is stereo. The streamer uses FFmpeg reconnect
settings for the BBC and FIP inputs, and if the pipeline still exits after a
long-running interruption, it refreshes the BBC stream timestamp and restarts.
The final mix is timestamp-smoothed before MP3 encoding to reduce short player
underruns during long streams.
Set `AWS_RESTART_DELAY_SECONDS` in `.env` if you want a longer or shorter pause
between restart attempts.

## 5. Smoke Tests

From your laptop or the EC2 instance:

```
curl -I https://PUBLIC_HOST/the-radio.mp3
```

For audio validation, use the `ffprobe` installed inside the streamer image:

```
docker compose exec streamer ffprobe https://PUBLIC_HOST/the-radio.mp3
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
STREAM_URL=https://PUBLIC_HOST/the-radio.mp3
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
- Port `8000` should stay closed publicly.
- If the stream is silent, inspect `docker compose logs streamer`.
- If HTTPS fails, inspect `docker compose logs caddy`.
- If the Icecast source password changes, restart the stack:

```
docker compose up -d
```
