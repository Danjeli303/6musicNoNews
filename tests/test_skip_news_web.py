import io
import os
import stat
import subprocess
import tempfile
import time
import unittest
from collections import deque
from pathlib import Path

import skip_news_web


ROOT = Path(__file__).resolve().parents[1]


class UrlTests(unittest.TestCase):
    def test_accepts_bbc_sounds_programme_urls(self):
        self.assertEqual(
            skip_news_web.parse_bbc_sounds_url(
                "https://www.bbc.co.uk/sounds/play/m0030yw7"
            ),
            "m0030yw7",
        )
        self.assertEqual(
            skip_news_web.parse_bbc_sounds_url(
                "https://bbc.co.uk/sounds/play/b0123456?at_mid=example"
            ),
            "b0123456",
        )

    def test_rejects_non_bbc_or_non_programme_urls(self):
        invalid = [
            "https://example.com/sounds/play/m0030yw7",
            "https://www.bbc.co.uk/sounds/brand/m0030yw7",
            "https://www.bbc.co.uk/sounds/play/not-a-pid",
            "javascript:alert(1)",
        ]
        for value in invalid:
            with self.subTest(value=value):
                self.assertIsNone(skip_news_web.parse_bbc_sounds_url(value))


class ProgressTests(unittest.TestCase):
    def test_splits_carriage_return_progress(self):
        stream = io.StringIO("1%\r2%\rDone\n")
        self.assertEqual(
            list(skip_news_web.iter_process_messages(stream)), ["1%", "2%", "Done"]
        )

    def test_maps_download_and_processing_progress(self):
        with tempfile.TemporaryDirectory() as output_dir:
            store = skip_news_web.JobStore(ROOT / "get_iplayer_skip_news.sh", output_dir)
            job_id = "test"
            store.jobs[job_id] = {
                "stage": "queued",
                "logs": deque(maxlen=80),
                "message": "",
            }
            store._handle_message(job_id, "STAGE=download")
            store._handle_message(job_id, " 50.0% of ~100 MB")
            self.assertEqual(store.snapshot(job_id)["progress"], 24)
            store._handle_message(job_id, "STAGE=process")
            store._handle_message(job_id, "Processed: 50%")
            self.assertEqual(store.snapshot(job_id)["progress"], 73)


class WrapperTests(unittest.TestCase):
    @staticmethod
    def _write_executable(path, body):
        path.write_text(body, encoding="utf-8")
        path.chmod(path.stat().st_mode | stat.S_IXUSR)

    def test_wrapper_downloads_processes_and_cleans_temporary_files(self):
        with tempfile.TemporaryDirectory() as temp_root:
            root = Path(temp_root)
            fake_get_iplayer = root / "get_iplayer"
            fake_skipper = root / "skip_news"
            output_dir = root / "output"
            temp_dir = root / "tmp"
            temp_dir.mkdir()

            self._write_executable(
                fake_get_iplayer,
                """#!/bin/sh
set -eu
out=
prefix=
for arg in "$@"; do
    case "$arg" in
        --output=*) out=${arg#--output=} ;;
        --file-prefix=*) prefix=${arg#--file-prefix=} ;;
    esac
done
printf ' 50.0%% of ~1 MB\n'
printf 'fake audio' > "$out/$prefix.m4a"
""",
            )
            self._write_executable(
                fake_skipper,
                """#!/bin/sh
set -eu
printf 'Processed: 50%%\rProcessed: 100%%\n'
cp "$1" "$2"
""",
            )

            environment = os.environ.copy()
            environment.update(
                {
                    "GET_IPLAYER_BIN": str(fake_get_iplayer),
                    "SKIP_NEWS_SCRIPT": str(fake_skipper),
                    "TMPDIR": str(temp_dir),
                }
            )
            completed = subprocess.run(
                [
                    str(ROOT / "get_iplayer_skip_news.sh"),
                    "https://www.bbc.co.uk/sounds/play/m0030yw7",
                    str(output_dir),
                ],
                check=True,
                capture_output=True,
                text=True,
                env=environment,
            )

            result = output_dir / "m0030yw7_newsskip.m4a"
            self.assertEqual(result.read_text(encoding="utf-8"), "fake audio")
            self.assertIn("STAGE=download", completed.stdout)
            self.assertIn("STAGE=process", completed.stdout)
            self.assertIn("STAGE=complete", completed.stdout)
            self.assertIn("OUTPUT_FILE=" + str(result), completed.stdout)
            self.assertEqual(list(temp_dir.iterdir()), [])

            subprocess.run(
                [
                    str(ROOT / "get_iplayer_skip_news.sh"),
                    "https://www.bbc.co.uk/sounds/play/m0030yw7",
                    str(output_dir),
                ],
                check=True,
                capture_output=True,
                text=True,
                env=environment,
            )
            self.assertEqual(
                (output_dir / "m0030yw7_newsskip_2.m4a").read_text(
                    encoding="utf-8"
                ),
                "fake audio",
            )

    def test_wrapper_rejects_untrusted_urls_before_running_downloader(self):
        completed = subprocess.run(
            [
                str(ROOT / "get_iplayer_skip_news.sh"),
                "https://example.com/sounds/play/m0030yw7",
            ],
            capture_output=True,
            text=True,
        )
        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("BBC Sounds programme URL", completed.stderr)


class JobStoreTests(unittest.TestCase):
    def test_completed_job_exposes_download_url(self):
        with tempfile.TemporaryDirectory() as temp_root:
            root = Path(temp_root)
            script = root / "processor"
            output_dir = root / "output"
            output_dir.mkdir()
            result = output_dir / "m0030yw7_newsskip.m4a"
            self._write_processor(script, result)

            store = skip_news_web.JobStore(script, output_dir)
            job = store.create("https://www.bbc.co.uk/sounds/play/m0030yw7")
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                snapshot = store.snapshot(job["id"])
                if snapshot["status"] in {"complete", "failed"}:
                    break
                time.sleep(0.02)

            self.assertEqual(snapshot["status"], "complete", snapshot["logs"])
            self.assertEqual(snapshot["progress"], 100)
            self.assertEqual(snapshot["filename"], result.name)
            self.assertEqual(snapshot["download_url"], "/downloads/" + result.name)

    @staticmethod
    def _write_processor(path, result):
        body = f"""#!/bin/sh
set -eu
printf 'STAGE=download\\n'
printf '%s\\n' ' 75.0% of ~1 MB'
printf 'STAGE=process\\n'
printf '%s\\n' 'Processed: 100%'
printf 'audio' > "$2/{result.name}"
printf '%s\\n' 'OUTPUT_FILE={result}'
printf 'STAGE=complete\\n'
"""
        path.write_text(body, encoding="utf-8")
        path.chmod(path.stat().st_mode | stat.S_IXUSR)


class DeploymentTests(unittest.TestCase):
    def test_caddy_routes_hls_and_web_on_the_same_host(self):
        caddyfile = (ROOT / "docker/caddy/Caddyfile").read_text(encoding="utf-8")
        self.assertIn("handle_path /hls/*", caddyfile)
        self.assertIn("reverse_proxy news-skipper-web:8080", caddyfile)

    def test_compose_services_share_one_built_image(self):
        compose = (ROOT / "docker-compose.yml").read_text(encoding="utf-8")
        self.assertEqual(compose.count("image: six-music-skipper:local"), 2)
        self.assertEqual(compose.count("pull_policy: build"), 2)
        self.assertEqual(compose.count("target: skipper-runtime"), 2)
        self.assertNotIn("SKIP_NEWS_WEB_PORT", compose)


if __name__ == "__main__":
    unittest.main()
