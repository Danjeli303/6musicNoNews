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
import uuid
from collections import deque
from datetime import datetime, timezone
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import quote, unquote, urlsplit


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


class ActiveJobError(RuntimeError):
    """Raised when deletion is requested for a job that is still processing."""


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
                not output_path.is_file()
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
            self.jobs[job_id] = {
                "id": job_id,
                "url": None,
                "pid": pid,
                "title": "BBC Sounds programme " + pid if pid else output_path.stem,
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
                self._update(
                    job_id,
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
            "img-src 'self' data:; font-src 'self' data:; connect-src 'self'; "
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

    def _send_download(self, encoded_name):
        filename = unquote(encoded_name)
        if not filename or filename != Path(filename).name:
            self.send_error(HTTPStatus.NOT_FOUND)
            return
        file_path = (self.server.job_store.output_dir / filename).resolve()
        if file_path.parent != self.server.job_store.output_dir or not file_path.is_file():
            self.send_error(HTTPStatus.NOT_FOUND)
            return
        self.send_response(HTTPStatus.OK)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(file_path.stat().st_size))
        self.send_header(
            "Content-Disposition", "attachment; filename*=UTF-8''" + quote(filename)
        )
        self.send_header("Cache-Control", "private, no-store")
        self._security_headers()
        self.end_headers()
        with file_path.open("rb") as source:
            shutil.copyfileobj(source, self.wfile)

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
        elif path.startswith("/api/jobs/"):
            job_id = path.rsplit("/", 1)[-1]
            job = self.server.job_store.snapshot(job_id)
            if job is None:
                self._send_json(HTTPStatus.NOT_FOUND, {"error": "Job not found."})
            else:
                self._send_json(HTTPStatus.OK, job)
        elif path.startswith("/downloads/"):
            self._send_download(path[len("/downloads/") :])
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
