#!/usr/bin/env python3
"""Local web interface for downloading and processing BBC Sounds programmes."""

import argparse
import hashlib
import json
import mimetypes
import os
import re
import shutil
import subprocess
import threading
import time
import uuid
from collections import deque
from datetime import datetime, timezone
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import quote, unquote, urlsplit
from urllib.request import Request, urlopen


BASE_DIR = Path(__file__).resolve().parent
WEB_DIR = BASE_DIR / "web"
DEFAULT_SCRIPT = BASE_DIR / "get_iplayer_skip_news.sh"
PID_PATTERN = re.compile(r"^[a-z0-9]{8}$")
ANSI_PATTERN = re.compile(r"\x1b\[[0-?]*[ -/]*[@-~]")
DOWNLOAD_PROGRESS_PATTERN = re.compile(r"(?:^|\s)(\d{1,3}(?:\.\d+)?)%")
PROCESS_PROGRESS_PATTERN = re.compile(r"Processed:\s*(\d{1,3})%")
MEDIA_EXTENSIONS = {
    ".flac",
    ".m4a",
    ".m4b",
    ".mp3",
    ".mp4",
    ".oga",
    ".ogg",
    ".opus",
    ".wav",
}
OUTPUT_PID_PATTERN = re.compile(r"^([a-z0-9]{8})_newsskip(?:_\d+)?$")
BYTE_RANGE_PATTERN = re.compile(r"^bytes=(\d*)-(\d*)$")
MEDIA_CONTENT_TYPES = {
    ".flac": "audio/flac",
    ".m4a": "audio/mp4",
    ".m4b": "audio/mp4",
    ".mp3": "audio/mpeg",
    ".mp4": "audio/mp4",
    ".oga": "audio/ogg",
    ".ogg": "audio/ogg",
    ".opus": "audio/ogg",
    ".wav": "audio/wav",
}
BBC_NOW_PLAYING_URL = (
    "https://rms.api.bbc.co.uk/v2/services/bbc_6music/segments/latest"
    "?experience=domestic&offset=0&limit=4"
)
BBC_IMAGE_HOST = "ichef.bbci.co.uk"


class ActiveJobError(RuntimeError):
    """Raised when deletion is requested for a job that is still processing."""


def _clean_text(value, limit=200):
    if not isinstance(value, str):
        return None
    clean = " ".join(value.split())
    return clean[:limit] if clean else None


def _bbc_image_url(value):
    value = _clean_text(value, limit=1000)
    if not value:
        return None
    parsed = urlsplit(value)
    if parsed.scheme != "https" or parsed.hostname != BBC_IMAGE_HOST:
        return None
    return value.replace("{recipe}", "640x640")


def bbc_now_playing_from_payload(payload):
    """Normalize the BBC Radio Metadata Service's latest music segment."""
    if not isinstance(payload, dict) or not isinstance(payload.get("data"), list):
        raise ValueError("The BBC metadata response was invalid.")

    tracks = [
        item
        for item in payload["data"]
        if isinstance(item, dict) and item.get("segment_type") == "music"
    ]
    current = next(
        (
            item
            for item in tracks
            if isinstance(item.get("offset"), dict)
            and item["offset"].get("now_playing") is True
        ),
        None,
    )
    item = current or (tracks[0] if tracks else None)
    if item is None:
        return {
            "available": False,
            "now_playing": False,
            "station": "BBC Radio 6 Music",
            "updated_at": datetime.now(timezone.utc).isoformat(),
        }

    titles = item.get("titles") if isinstance(item.get("titles"), dict) else {}
    offset = item.get("offset") if isinstance(item.get("offset"), dict) else {}
    artist = _clean_text(titles.get("primary"))
    title = _clean_text(titles.get("secondary"))
    if not artist and not title:
        return {
            "available": False,
            "now_playing": False,
            "station": "BBC Radio 6 Music",
            "updated_at": datetime.now(timezone.utc).isoformat(),
        }
    return {
        "available": True,
        "now_playing": current is not None,
        "station": "BBC Radio 6 Music",
        "artist": artist,
        "title": title,
        "image_url": _bbc_image_url(item.get("image_url")),
        "label": _clean_text(offset.get("label"), limit=80),
        "updated_at": datetime.now(timezone.utc).isoformat(),
    }


class BBCNowPlayingService:
    """Fetch and briefly cache BBC 6 Music track metadata."""

    def __init__(self, endpoint=BBC_NOW_PLAYING_URL, cache_seconds=15):
        self.endpoint = endpoint
        self.cache_seconds = cache_seconds
        self.lock = threading.Lock()
        self.cached = None
        self.cached_at = 0.0

    def get(self):
        with self.lock:
            now = time.monotonic()
            if self.cached is not None and now - self.cached_at < self.cache_seconds:
                return dict(self.cached)
            request = Request(
                self.endpoint,
                headers={
                    "Accept": "application/json",
                    "User-Agent": "6MusicNewsSkipper/1.0",
                },
            )
            try:
                with urlopen(request, timeout=5) as response:
                    raw = response.read(1_000_001)
                if len(raw) > 1_000_000:
                    raise ValueError("The BBC metadata response was too large.")
                result = bbc_now_playing_from_payload(json.loads(raw.decode("utf-8")))
            except Exception:
                if self.cached is not None:
                    stale = dict(self.cached)
                    stale["stale"] = True
                    return stale
                raise
            self.cached = result
            self.cached_at = now
            return dict(result)


def parse_bbc_sounds_url(value):
    """Return the PID from a canonical BBC Sounds programme URL."""
    try:
        parsed = urlsplit(value.strip())
    except (TypeError, ValueError):
        return None

    if parsed.scheme not in {"http", "https"}:
        return None
    if (parsed.hostname or "").lower() not in {"bbc.co.uk", "www.bbc.co.uk"}:
        return None

    parts = [part for part in parsed.path.split("/") if part]
    if len(parts) != 3 or parts[:2] != ["sounds", "play"]:
        return None
    return parts[2] if PID_PATTERN.fullmatch(parts[2]) else None


def iter_process_messages(stream):
    """Split process output on both newline and carriage-return progress updates."""
    pending = []
    while True:
        char = stream.read(1)
        if char == "":
            if pending:
                yield "".join(pending)
            return
        if char in "\r\n":
            if pending:
                yield "".join(pending)
                pending = []
        else:
            pending.append(char)


def _metadata_value(tags, *names, limit=500):
    for name in names:
        value = tags.get(name)
        if value is None:
            continue
        clean = " ".join(str(value).split())
        if clean:
            return clean[:limit]
    return None


def media_details_from_tags(tags, pid=None, filename=None):
    """Return display metadata from ffprobe format tags."""
    normalized = {str(key).lower(): value for key, value in (tags or {}).items()}
    fallback = "BBC Sounds programme " + pid if pid else Path(filename or "Programme").stem
    title = _metadata_value(
        normalized, "title", "episode", "episode_title", limit=200
    ) or fallback
    artist = _metadata_value(
        normalized, "artist", "album_artist", "albumartist", "show", limit=160
    )
    album = _metadata_value(
        normalized, "album", "show", "series", "programme", limit=200
    )
    description = _metadata_value(
        normalized, "description", "synopsis", "comment", "desc", limit=800
    )
    if artist and artist.casefold() not in title.casefold():
        display_title = artist + " - " + title
    else:
        display_title = title
    return {
        "title": title,
        "artist": artist,
        "album": album,
        "description": description,
        "display_title": display_title,
    }


class JobStore:
    def __init__(self, script_path, output_dir):
        self.script_path = Path(script_path).resolve()
        self.output_dir = Path(output_dir).resolve()
        self.output_dir.mkdir(parents=True, exist_ok=True)
        self.jobs = {}
        self.lock = threading.Lock()
        self.processing_lock = threading.Lock()
        self._discover_existing_outputs()

    @staticmethod
    def _snapshot_job(job):
        snapshot = dict(job)
        snapshot["logs"] = list(job["logs"])
        return snapshot

    def _discover_existing_outputs(self):
        for output_path in self.output_dir.iterdir():
            if (
                output_path.is_symlink()
                or not output_path.is_file()
                or output_path.suffix.lower() not in MEDIA_EXTENSIONS
            ):
                continue
            match = OUTPUT_PID_PATTERN.fullmatch(output_path.stem)
            pid = match.group(1) if match else None
            digest = hashlib.sha256(output_path.name.encode("utf-8")).hexdigest()
            job_id = "archive-" + digest[:16]
            modified = datetime.fromtimestamp(
                output_path.stat().st_mtime, timezone.utc
            ).isoformat()
            media_details = self._media_details(output_path, pid)
            self.jobs[job_id] = {
                "id": job_id,
                "url": None,
                "pid": pid,
                **media_details,
                "status": "complete",
                "stage": "complete",
                "stage_label": "Ready to download",
                "progress": 100,
                "stage_progress": 100,
                "message": "This previously processed programme is ready.",
                "logs": deque(maxlen=80),
                "filename": output_path.name,
                "file_size": output_path.stat().st_size,
                "download_url": "/downloads/" + quote(output_path.name),
                "created_at": modified,
            }

    @staticmethod
    def _artwork_path(output_path):
        return output_path.with_name(output_path.stem + ".artwork.jpg")

    def _extract_artwork(self, output_path):
        artwork_path = self._artwork_path(output_path)
        if artwork_path.is_file() and artwork_path.stat().st_size:
            return artwork_path
        ffmpeg = shutil.which("ffmpeg")
        if not ffmpeg:
            return None
        temporary_path = artwork_path.with_name(artwork_path.name + ".tmp")
        try:
            result = subprocess.run(
                [
                    ffmpeg,
                    "-v",
                    "error",
                    "-y",
                    "-i",
                    str(output_path),
                    "-map",
                    "0:v:0",
                    "-frames:v",
                    "1",
                    "-an",
                    "-c:v",
                    "mjpeg",
                    "-f",
                    "image2",
                    str(temporary_path),
                ],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                timeout=30,
                check=False,
            )
            if result.returncode == 0 and temporary_path.is_file():
                temporary_path.replace(artwork_path)
                return artwork_path
        except (OSError, subprocess.SubprocessError):
            pass
        finally:
            temporary_path.unlink(missing_ok=True)
        return None

    def _media_details(self, output_path, pid):
        tags = {}
        ffprobe = shutil.which("ffprobe")
        if ffprobe:
            try:
                result = subprocess.run(
                    [
                        ffprobe,
                        "-v",
                        "error",
                        "-show_entries",
                        "format_tags",
                        "-of",
                        "json",
                        str(output_path),
                    ],
                    capture_output=True,
                    text=True,
                    encoding="utf-8",
                    errors="replace",
                    timeout=15,
                    check=False,
                )
                if result.returncode == 0:
                    payload = json.loads(result.stdout)
                    tags = payload.get("format", {}).get("tags", {})
            except (OSError, subprocess.SubprocessError, json.JSONDecodeError):
                pass

        details = media_details_from_tags(tags, pid, output_path.name)
        artwork_path = self._extract_artwork(output_path)
        details.update(
            {
                "media_url": "/media/" + quote(output_path.name),
                "media_type": MEDIA_CONTENT_TYPES.get(
                    output_path.suffix.lower(), "application/octet-stream"
                ),
                "artwork_url": (
                    "/artwork/" + quote(artwork_path.name) if artwork_path else None
                ),
            }
        )
        return details

    def create(self, sounds_url):
        pid = parse_bbc_sounds_url(sounds_url)
        if not pid:
            raise ValueError(
                "Enter a BBC Sounds programme link like "
                "https://www.bbc.co.uk/sounds/play/m0030yw7"
            )

        job_id = uuid.uuid4().hex
        job = {
            "id": job_id,
            "url": sounds_url.strip(),
            "pid": pid,
            "title": "BBC Sounds programme " + pid,
            "artist": None,
            "album": None,
            "description": None,
            "display_title": "BBC Sounds programme " + pid,
            "status": "queued",
            "stage": "queued",
            "stage_label": "Waiting to start",
            "progress": 0,
            "stage_progress": None,
            "message": "Your programme is queued.",
            "logs": deque(maxlen=80),
            "filename": None,
            "file_size": None,
            "download_url": None,
            "media_url": None,
            "media_type": None,
            "artwork_url": None,
            "created_at": datetime.now(timezone.utc).isoformat(),
        }
        with self.lock:
            self.jobs[job_id] = job
        threading.Thread(target=self._run, args=(job_id,), daemon=True).start()
        return self.snapshot(job_id)

    def snapshot(self, job_id):
        with self.lock:
            job = self.jobs.get(job_id)
            if job is None:
                return None
            return self._snapshot_job(job)

    def list_jobs(self):
        with self.lock:
            snapshots = [self._snapshot_job(job) for job in self.jobs.values()]
        active = [job for job in snapshots if job["status"] in {"queued", "running"}]
        history = [job for job in snapshots if job["status"] in {"complete", "failed"}]
        active.sort(key=lambda job: job["created_at"])
        history.sort(key=lambda job: job["created_at"], reverse=True)
        return {"active": active, "history": history}

    def remove(self, job_id):
        with self.lock:
            job = self.jobs.get(job_id)
            if job is None:
                return None
            if job["status"] in {"queued", "running"}:
                raise ActiveJobError("A programme cannot be removed while it is processing.")
            filename = job.get("filename")

        if filename:
            output_path = (self.output_dir / filename).resolve()
            if output_path.parent != self.output_dir or output_path.name != filename:
                raise RuntimeError("The stored output filename is invalid.")
            output_path.unlink(missing_ok=True)
            output_path.with_suffix(".log").unlink(missing_ok=True)
            self._artwork_path(output_path).unlink(missing_ok=True)

        with self.lock:
            removed = self.jobs.pop(job_id, None)
            return self._snapshot_job(removed) if removed else None

    def _update(self, job_id, **values):
        with self.lock:
            self.jobs[job_id].update(values)

    def _append_log(self, job_id, message):
        clean = ANSI_PATTERN.sub("", message).strip()
        if not clean or clean.startswith(("STAGE=", "OUTPUT_FILE=")):
            return
        with self.lock:
            logs = self.jobs[job_id]["logs"]
            if not logs or logs[-1] != clean:
                logs.append(clean)
            self.jobs[job_id]["message"] = clean

    def _handle_message(self, job_id, message):
        if message == "STAGE=download":
            self._update(
                job_id,
                stage="download",
                stage_label="Downloading from BBC Sounds",
                progress=2,
                stage_progress=0,
                message="Connecting to BBC Sounds…",
            )
            return
        if message == "STAGE=process":
            self._update(
                job_id,
                stage="process",
                stage_label="Removing news",
                progress=48,
                stage_progress=0,
                message="The programme is downloaded. Removing scheduled news…",
            )
            return
        if message == "STAGE=complete":
            return
        if message.startswith("OUTPUT_FILE="):
            output_path = Path(message.split("=", 1)[1]).resolve()
            if output_path.parent != self.output_dir or not output_path.is_file():
                raise RuntimeError("The processor returned an invalid output file.")
            filename = output_path.name
            self._update(
                job_id,
                filename=filename,
                file_size=output_path.stat().st_size,
                download_url="/downloads/" + quote(filename),
            )
            return

        snapshot = self.snapshot(job_id)
        if snapshot["stage"] == "download":
            match = DOWNLOAD_PROGRESS_PATTERN.search(message)
            if match:
                percent = min(100, max(0, int(float(match.group(1)))))
                self._update(
                    job_id,
                    stage_progress=percent,
                    progress=2 + int(percent * 0.44),
                )
        elif snapshot["stage"] == "process":
            match = PROCESS_PROGRESS_PATTERN.search(message)
            if match:
                percent = min(100, max(0, int(match.group(1))))
                self._update(
                    job_id,
                    stage_progress=percent,
                    progress=48 + int(percent * 0.51),
                )
        self._append_log(job_id, message)

    def _run(self, job_id):
        with self.processing_lock:
            self._update(
                job_id,
                status="running",
                stage="starting",
                stage_label="Starting",
                message="Preparing the downloader…",
                progress=1,
            )
            job = self.snapshot(job_id)
            command = [str(self.script_path), job["url"], str(self.output_dir)]
            try:
                process = subprocess.Popen(
                    command,
                    cwd=str(BASE_DIR),
                    stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT,
                    text=True,
                    encoding="utf-8",
                    errors="replace",
                    bufsize=0,
                )
                if process.stdout is None:
                    raise RuntimeError("Could not read processor progress.")
                with process.stdout:
                    for message in iter_process_messages(process.stdout):
                        self._handle_message(job_id, message)
                return_code = process.wait()
                completed = self.snapshot(job_id)
                if return_code != 0:
                    raise RuntimeError(
                        completed["message"] or "Programme processing failed."
                    )
                if not completed["filename"]:
                    raise RuntimeError("Processing finished without an output file.")
                output_path = self.output_dir / completed["filename"]
                media_details = self._media_details(output_path, completed["pid"])
                self._update(
                    job_id,
                    **media_details,
                    status="complete",
                    stage="complete",
                    stage_label="Ready to download",
                    progress=100,
                    stage_progress=100,
                    message="Your news-skipped programme is ready.",
                )
            except Exception as exc:  # Surface failures to the polling client.
                self._append_log(job_id, str(exc))
                self._update(
                    job_id,
                    status="failed",
                    stage="failed",
                    stage_label="Could not finish",
                    message=str(exc),
                )


class SkipNewsHandler(BaseHTTPRequestHandler):
    server_version = "SkipNewsWeb/1.0"

    def log_message(self, fmt, *args):
        print("%s - %s" % (self.address_string(), fmt % args), flush=True)

    def _security_headers(self):
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("X-Frame-Options", "DENY")
        self.send_header("Referrer-Policy", "no-referrer")
        self.send_header(
            "Content-Security-Policy",
            "default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; "
            "img-src 'self' data: https://ichef.bbci.co.uk; "
            "font-src 'self' data:; connect-src 'self'; "
            "media-src 'self' blob:; worker-src 'self' blob:; base-uri 'none'; "
            "form-action 'self'; frame-ancestors 'none'",
        )

    def _send_json(self, status, payload):
        body = json.dumps(payload).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self._security_headers()
        self.end_headers()
        self.wfile.write(body)

    def _send_static(self, relative_path):
        file_path = (WEB_DIR / relative_path).resolve()
        web_dir = WEB_DIR.resolve()
        if web_dir not in file_path.parents or not file_path.is_file():
            self.send_error(HTTPStatus.NOT_FOUND)
            return
        body = file_path.read_bytes()
        content_type = mimetypes.guess_type(file_path.name)[0] or "application/octet-stream"
        self.send_response(HTTPStatus.OK)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-cache")
        self._security_headers()
        self.end_headers()
        self.wfile.write(body)

    def _resolve_output_file(self, encoded_name):
        filename = unquote(encoded_name)
        if not filename or filename != Path(filename).name:
            return None
        file_path = (self.server.job_store.output_dir / filename).resolve()
        if file_path.parent != self.server.job_store.output_dir or not file_path.is_file():
            return None
        return file_path

    def _send_download(self, encoded_name):
        file_path = self._resolve_output_file(encoded_name)
        if file_path is None or file_path.suffix.lower() not in MEDIA_EXTENSIONS:
            self.send_error(HTTPStatus.NOT_FOUND)
            return
        self.send_response(HTTPStatus.OK)
        self.send_header(
            "Content-Type",
            MEDIA_CONTENT_TYPES.get(file_path.suffix.lower(), "application/octet-stream"),
        )
        self.send_header("Content-Length", str(file_path.stat().st_size))
        self.send_header(
            "Content-Disposition",
            "attachment; filename*=UTF-8''" + quote(file_path.name),
        )
        self.send_header("Cache-Control", "private, no-store")
        self._security_headers()
        self.end_headers()
        with file_path.open("rb") as source:
            shutil.copyfileobj(source, self.wfile)

    def _send_media(self, encoded_name):
        file_path = self._resolve_output_file(encoded_name)
        if file_path is None or file_path.suffix.lower() not in MEDIA_EXTENSIONS:
            self.send_error(HTTPStatus.NOT_FOUND)
            return

        size = file_path.stat().st_size
        start = 0
        end = size - 1
        status = HTTPStatus.OK
        range_header = self.headers.get("Range")
        if range_header:
            match = BYTE_RANGE_PATTERN.fullmatch(range_header.strip())
            if not match or not any(match.groups()):
                self._send_range_error(size)
                return
            start_text, end_text = match.groups()
            if start_text:
                start = int(start_text)
                end = min(int(end_text), size - 1) if end_text else size - 1
            else:
                suffix_length = int(end_text)
                if suffix_length < 1:
                    self._send_range_error(size)
                    return
                start = max(0, size - suffix_length)
            if start >= size or end < start:
                self._send_range_error(size)
                return
            status = HTTPStatus.PARTIAL_CONTENT

        content_length = end - start + 1
        self.send_response(status)
        self.send_header(
            "Content-Type",
            MEDIA_CONTENT_TYPES.get(file_path.suffix.lower(), "application/octet-stream"),
        )
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("Content-Length", str(content_length))
        if status == HTTPStatus.PARTIAL_CONTENT:
            self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
        self.send_header("Cache-Control", "private, no-cache")
        self._security_headers()
        self.end_headers()
        remaining = content_length
        with file_path.open("rb") as source:
            source.seek(start)
            while remaining:
                chunk = source.read(min(64 * 1024, remaining))
                if not chunk:
                    break
                try:
                    self.wfile.write(chunk)
                except (BrokenPipeError, ConnectionResetError):
                    break
                remaining -= len(chunk)

    def _send_range_error(self, size):
        self.send_response(HTTPStatus.REQUESTED_RANGE_NOT_SATISFIABLE)
        self.send_header("Content-Range", f"bytes */{size}")
        self.send_header("Content-Length", "0")
        self._security_headers()
        self.end_headers()

    def _send_artwork(self, encoded_name):
        file_path = self._resolve_output_file(encoded_name)
        if file_path is None or not file_path.name.endswith(".artwork.jpg"):
            self.send_error(HTTPStatus.NOT_FOUND)
            return
        body = file_path.read_bytes()
        self.send_response(HTTPStatus.OK)
        self.send_header("Content-Type", "image/jpeg")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "private, max-age=3600")
        self._security_headers()
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        path = urlsplit(self.path).path
        if path == "/":
            self._send_static("index.html")
        elif path == "/app.js":
            self._send_static("app.js")
        elif path == "/styles.css":
            self._send_static("styles.css")
        elif path.startswith("/vendor/"):
            self._send_static(path.lstrip("/"))
        elif path == "/api/jobs":
            self._send_json(HTTPStatus.OK, self.server.job_store.list_jobs())
        elif path == "/api/now-playing":
            try:
                payload = self.server.now_playing_service.get()
            except Exception:
                self._send_json(
                    HTTPStatus.BAD_GATEWAY,
                    {"error": "BBC 6 Music track information is unavailable."},
                )
            else:
                self._send_json(HTTPStatus.OK, payload)
        elif path.startswith("/api/jobs/"):
            job_id = path.rsplit("/", 1)[-1]
            job = self.server.job_store.snapshot(job_id)
            if job is None:
                self._send_json(HTTPStatus.NOT_FOUND, {"error": "Job not found."})
            else:
                self._send_json(HTTPStatus.OK, job)
        elif path.startswith("/downloads/"):
            self._send_download(path[len("/downloads/") :])
        elif path.startswith("/media/"):
            self._send_media(path[len("/media/") :])
        elif path.startswith("/artwork/"):
            self._send_artwork(path[len("/artwork/") :])
        else:
            self.send_error(HTTPStatus.NOT_FOUND)

    def do_POST(self):
        if urlsplit(self.path).path != "/api/jobs":
            self.send_error(HTTPStatus.NOT_FOUND)
            return
        try:
            length = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            length = 0
        if length < 1 or length > 4096:
            self._send_json(HTTPStatus.BAD_REQUEST, {"error": "Invalid request."})
            return
        try:
            data = json.loads(self.rfile.read(length).decode("utf-8"))
            sounds_url = data.get("url", "")
            if not isinstance(sounds_url, str):
                raise ValueError("Invalid URL.")
            job = self.server.job_store.create(sounds_url)
        except (json.JSONDecodeError, UnicodeDecodeError):
            self._send_json(HTTPStatus.BAD_REQUEST, {"error": "Invalid request."})
            return
        except ValueError as exc:
            self._send_json(HTTPStatus.BAD_REQUEST, {"error": str(exc)})
            return
        self._send_json(HTTPStatus.ACCEPTED, job)

    def do_DELETE(self):
        path = urlsplit(self.path).path
        if not path.startswith("/api/jobs/"):
            self.send_error(HTTPStatus.NOT_FOUND)
            return
        job_id = path.rsplit("/", 1)[-1]
        try:
            removed = self.server.job_store.remove(job_id)
        except ActiveJobError as exc:
            self._send_json(HTTPStatus.CONFLICT, {"error": str(exc)})
            return
        except (OSError, RuntimeError):
            self._send_json(
                HTTPStatus.INTERNAL_SERVER_ERROR,
                {"error": "The programme could not be removed from the server."},
            )
            return
        if removed is None:
            self._send_json(HTTPStatus.NOT_FOUND, {"error": "Job not found."})
            return
        self._send_json(HTTPStatus.OK, {"removed": True, "id": job_id})


class SkipNewsServer(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, address, job_store):
        super().__init__(address, SkipNewsHandler)
        self.job_store = job_store
        self.now_playing_service = BBCNowPlayingService(
            os.environ.get("BBC_NOW_PLAYING_URL", BBC_NOW_PLAYING_URL)
        )


def build_parser():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default=os.environ.get("SKIP_NEWS_HOST", "127.0.0.1"))
    parser.add_argument(
        "--port", type=int, default=int(os.environ.get("SKIP_NEWS_PORT", "8080"))
    )
    parser.add_argument(
        "--output-dir",
        default=os.environ.get("SKIP_NEWS_OUTPUT_DIR", str(BASE_DIR / "downloads")),
    )
    parser.add_argument(
        "--script", default=os.environ.get("SKIP_NEWS_SCRIPT", str(DEFAULT_SCRIPT))
    )
    return parser


def main():
    args = build_parser().parse_args()
    job_store = JobStore(args.script, args.output_dir)
    server = SkipNewsServer((args.host, args.port), job_store)
    print("6 Music News Skipper: http://%s:%d" % (args.host, args.port), flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
