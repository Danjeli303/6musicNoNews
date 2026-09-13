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


class MediaMetadataTests(unittest.TestCase):
    def test_builds_programme_title_from_embedded_m4a_tags(self):
        details = skip_news_web.media_details_from_tags(
            {
                "artist": "Gilles Peterson",
                "title": (
                    "Carlos Icaza in the studio, Cassandra Wilson tribute, "
                    "words from anaiis"
                ),
                "album": "Gilles Peterson",
                "description": "A musical journey from BBC Radio 6 Music.",
            },
            pid="m0030yw7",
            filename="m0030yw7_newsskip.m4a",
        )

        self.assertEqual(
            details["display_title"],
            "Gilles Peterson - Carlos Icaza in the studio, Cassandra Wilson "
            "tribute, words from anaiis",
        )
        self.assertEqual(details["album"], "Gilles Peterson")
        self.assertEqual(
            details["description"], "A musical journey from BBC Radio 6 Music."
        )

    def test_uses_pid_fallback_when_tags_are_unavailable(self):
        details = skip_news_web.media_details_from_tags(
            {}, pid="m0030yw7", filename="m0030yw7_newsskip.m4a"
        )
        self.assertEqual(details["display_title"], "BBC Sounds programme m0030yw7")


class NowPlayingTests(unittest.TestCase):
    def test_normalizes_current_bbc_6_music_track(self):
        result = skip_news_web.bbc_now_playing_from_payload(
            {
                "data": [
                    {
                        "segment_type": "music",
                        "titles": {
                            "primary": "Factory Floor",
                            "secondary": "Upper Left",
                        },
                        "image_url": (
                            "https://ichef.bbci.co.uk/images/ic/"
                            "{recipe}/p01br4y8.jpg"
                        ),
                        "offset": {"label": "Now Playing", "now_playing": True},
                    }
                ]
            }
        )

        self.assertTrue(result["available"])
        self.assertTrue(result["now_playing"])
        self.assertEqual(result["artist"], "Factory Floor")
        self.assertEqual(result["title"], "Upper Left")
        self.assertEqual(
            result["image_url"],
            "https://ichef.bbci.co.uk/images/ic/640x640/p01br4y8.jpg",
        )

    def test_labels_latest_track_as_recent_when_nothing_is_current(self):
        result = skip_news_web.bbc_now_playing_from_payload(
            {
                "data": [
                    {
                        "segment_type": "music",
                        "titles": {"primary": "Artist", "secondary": "Track"},
                        "image_url": "https://example.com/untrusted.jpg",
                        "offset": {"label": "2 Minutes Ago", "now_playing": False},
                    }
                ]
            }
        )

        self.assertTrue(result["available"])
        self.assertFalse(result["now_playing"])
        self.assertIsNone(result["image_url"])

    def test_bounds_configured_metadata_delay(self):
        self.assertEqual(skip_news_web.now_playing_delay_seconds("24.5"), 24.5)
        self.assertEqual(skip_news_web.now_playing_delay_seconds("-3"), 0)
        self.assertEqual(skip_news_web.now_playing_delay_seconds("999"), 120)
        self.assertEqual(
            skip_news_web.now_playing_delay_seconds("invalid"),
            skip_news_web.DEFAULT_NOW_PLAYING_DELAY_SECONDS,
        )


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

    def test_packaged_skip_script_only_requires_make_when_binary_is_missing(self):
        script = (ROOT / "skip_6music_news.sh").read_text(encoding="utf-8")
        ensure_skipper = script.split("ensure_skipper() {", 1)[1].split("\n}", 1)[0]
        startup = script.split("INPUT=${INPUT_ARG", 1)[1]

        self.assertIn("require_command make", ensure_skipper)
        self.assertNotIn("require_command make", startup)

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
    def test_discovers_previously_processed_outputs(self):
        with tempfile.TemporaryDirectory() as temp_root:
            output_dir = Path(temp_root) / "output"
            output_dir.mkdir()
            result = output_dir / "m0030yw7_newsskip.m4a"
            result.write_bytes(b"previous audio")
            (output_dir / "m0030yw7_newsskip.log").write_text(
                "processing log", encoding="utf-8"
            )

            store = skip_news_web.JobStore(ROOT / "get_iplayer_skip_news.sh", output_dir)
            jobs = store.list_jobs()

            self.assertEqual(jobs["active"], [])
            self.assertEqual(len(jobs["history"]), 1)
            self.assertEqual(jobs["history"][0]["pid"], "m0030yw7")
            self.assertEqual(jobs["history"][0]["filename"], result.name)
            self.assertEqual(jobs["history"][0]["file_size"], len(b"previous audio"))
            self.assertEqual(jobs["history"][0]["media_url"], "/media/" + result.name)

    def test_removes_processed_audio_and_log(self):
        with tempfile.TemporaryDirectory() as temp_root:
            output_dir = Path(temp_root) / "output"
            output_dir.mkdir()
            result = output_dir / "m0030yw7_newsskip.m4a"
            log = output_dir / "m0030yw7_newsskip.log"
            artwork = output_dir / "m0030yw7_newsskip.artwork.jpg"
            result.write_bytes(b"audio")
            log.write_text("log", encoding="utf-8")
            artwork.write_bytes(b"artwork")
            store = skip_news_web.JobStore(ROOT / "get_iplayer_skip_news.sh", output_dir)
            archived = store.list_jobs()["history"][0]
            self.assertEqual(
                archived["artwork_url"],
                "/artwork/m0030yw7_newsskip.artwork.jpg",
            )

            removed = store.remove(archived["id"])

            self.assertEqual(removed["filename"], result.name)
            self.assertFalse(result.exists())
            self.assertFalse(log.exists())
            self.assertFalse(artwork.exists())
            self.assertEqual(store.list_jobs()["history"], [])

    def test_does_not_remove_an_active_job(self):
        with tempfile.TemporaryDirectory() as output_dir:
            store = skip_news_web.JobStore(ROOT / "get_iplayer_skip_news.sh", output_dir)
            store.jobs["active"] = {"status": "running", "filename": None}
            with self.assertRaises(skip_news_web.ActiveJobError):
                store.remove("active")

    def test_rejects_jobs_when_the_queue_is_full(self):
        with tempfile.TemporaryDirectory() as output_dir:
            store = skip_news_web.JobStore(ROOT / "get_iplayer_skip_news.sh", output_dir)
            store.jobs = {
                str(index): {"status": "queued"}
                for index in range(skip_news_web.MAX_ACTIVE_JOBS)
            }

            with self.assertRaisesRegex(ValueError, "Too many programmes"):
                store.create("https://www.bbc.co.uk/sounds/play/m0030yw7")

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
            self.assertEqual(snapshot["file_size"], len(b"audio"))
            self.assertEqual(snapshot["download_url"], "/downloads/" + result.name)
            self.assertEqual(snapshot["media_url"], "/media/" + result.name)

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
    def test_page_uses_simple_live_process_programmes_order(self):
        html = (ROOT / "web/index.html").read_text(encoding="utf-8")

        self.assertLess(html.index('id="live-stream"'), html.index('id="process"'))
        self.assertLess(html.index('id="process"'), html.index('id="programmes"'))
        self.assertIn('href="#live-stream" aria-current="page">Live Stream</a>', html)
        self.assertIn('href="#process">Process</a>', html)
        self.assertIn('href="#programmes">Programmes</a>', html)
        self.assertNotIn("Your activity", html)
        self.assertNotIn("Your programme.", html)
        self.assertNotIn("Without the news.", html)

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

    def test_live_player_is_bundled_and_uses_the_local_hls_stream(self):
        html = (ROOT / "web/index.html").read_text(encoding="utf-8")
        app = (ROOT / "web/app.js").read_text(encoding="utf-8")
        styles = (ROOT / "web/styles.css").read_text(encoding="utf-8")
        vendor_dir = ROOT / "web/vendor/video.js"

        self.assertIn('id="live-radio-player"', html)
        self.assertIn('src="/hls/radio6music_noNews.m3u8"', html)
        self.assertIn('src="/vendor/video.js/video.min.js"', html)
        self.assertIn('href="/vendor/video.js/video-js.min.css"', html)
        self.assertIn('window.videojs("live-radio-player"', app)
        self.assertIn("enforceExclusivePlayback", app)
        self.assertIn("autoplay: false", app)
        self.assertNotIn("<audio autoplay", html)
        self.assertIn('id="now-playing-title"', html)
        self.assertIn('fetch("/api/now-playing"', app)
        self.assertIn("setInterval(loadNowPlaying, 5000)", app)
        self.assertIn("display_delay_seconds", app)
        self.assertIn(
            ".history-player-wrap .video-js.vjs-layout-tiny .vjs-progress-control",
            styles,
        )
        self.assertIn(
            ".history-player-wrap .video-js.vjs-layout-x-small .vjs-progress-control",
            styles,
        )
        self.assertGreater((vendor_dir / "video.min.js").stat().st_size, 100_000)
        self.assertTrue((vendor_dir / "video-js.min.css").is_file())
        self.assertTrue((vendor_dir / "LICENSE").is_file())

    def test_favourites_use_a_shared_lastfm_compatible_store(self):
        html = (ROOT / "web/index.html").read_text(encoding="utf-8")
        favourites = (ROOT / "web/favourites.js").read_text(encoding="utf-8")
        favourites_page = (ROOT / "web/favourites.html").read_text(encoding="utf-8")

        self.assertIn('href="/favourites.html" target="_blank"', html)
        self.assertIn('id="now-playing-favourite"', html)
        self.assertIn('src="/favourites.js"', html)
        self.assertIn("skipper.favouriteTracks.v1", favourites)
        self.assertIn("https://www.last.fm/search?q=", favourites)
        self.assertIn('id="favourites-list"', favourites_page)


if __name__ == "__main__":
    unittest.main()
