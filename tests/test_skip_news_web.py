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


class TracklistTests(unittest.TestCase):
    TRACKLIST = """Example Show
Example episode
2026-09-14
https://www.bbc.co.uk/sounds/play/m0030yw7
Music
--------
00:00:16
Example Artist
Example Track
Release Title: Example Album
Record Label: Example Records
Duration: 00:00:02
--------
"""
    def test_parses_get_iplayer_track_information(self):
        tracks = skip_news_web.parse_get_iplayer_tracklist(
            self.TRACKLIST, pid="m0030yw7"
        )

        self.assertEqual(len(tracks), 1)
        self.assertEqual(tracks[0]["artist"], "Example Artist")
        self.assertEqual(tracks[0]["title"], "Example Track")
        self.assertEqual(tracks[0]["album"], "Example Album")
        self.assertEqual(tracks[0]["programme"], "Example Show")
        self.assertEqual(tracks[0]["presenter"], "Example episode")
        self.assertEqual(tracks[0]["start_seconds"], 16)
        self.assertEqual(tracks[0]["end_seconds"], 18)

    def test_embedded_programme_metadata_takes_precedence(self):
        track = skip_news_web.parse_get_iplayer_tracklist(
            self.TRACKLIST,
            programme="M4A Album Show",
            presenter="M4A Title Presenter",
        )[0]

        self.assertEqual(track["programme"], "M4A Album Show")
        self.assertEqual(track["presenter"], "M4A Title Presenter")

    def test_adjusts_track_offsets_using_actual_discarded_samples(self):
        tracks = skip_news_web.parse_get_iplayer_tracklist(self.TRACKLIST)
        adjusted = skip_news_web.adjust_tracklist_for_skips(
            tracks,
            """sample rate = 10
timeline: input_samples=100 output_samples=100 discarded_samples=0
timeline: input_samples=150 output_samples=100 discarded_samples=50
timeline: input_samples=250 output_samples=200 discarded_samples=50
""",
        )

        self.assertEqual(adjusted[0]["original_start_seconds"], 16)
        self.assertEqual(adjusted[0]["start_seconds"], 11)
        self.assertEqual(adjusted[0]["end_seconds"], 13)

    def test_only_moves_start_and_preserves_track_duration(self):
        track = {
            "original_start_seconds": 9,
            "start_seconds": 9,
            "duration_seconds": 10,
            "end_seconds": 19,
        }
        adjusted = skip_news_web.adjust_tracklist_for_skips(
            [track],
            """sample rate = 10
timeline: input_samples=100 output_samples=100 discarded_samples=0
timeline: input_samples=150 output_samples=100 discarded_samples=50
timeline: input_samples=250 output_samples=200 discarded_samples=50
""",
        )[0]

        self.assertEqual(adjusted["start_seconds"], 9)
        self.assertEqual(adjusted["end_seconds"], 19)
        self.assertEqual(
            adjusted["end_seconds"] - adjusted["start_seconds"],
            track["duration_seconds"],
        )


class FavouriteStoreTests(unittest.TestCase):
    def test_persists_lastfm_shaped_favourites_between_instances(self):
        with tempfile.TemporaryDirectory() as output_dir:
            path = Path(output_dir) / "favourites.json"
            store = skip_news_web.FavouriteStore(path)
            saved = store.add(
                {
                    "name": "Example Track",
                    "artist": {"name": "Example Artist"},
                    "programme": "Lauren Laverne",
                    "presenter": "Lauren Laverne",
                    "programme_pid": "m00318j8",
                }
            )

            self.assertEqual(saved["name"], "Example Track")
            reloaded = skip_news_web.FavouriteStore(path)
            self.assertEqual(reloaded.list()["tracks"][0]["programme"], "Lauren Laverne")
            self.assertTrue(reloaded.remove(saved))
            self.assertEqual(skip_news_web.FavouriteStore(path).list(), {"tracks": []})

    def test_readding_a_track_updates_context_without_duplication(self):
        with tempfile.TemporaryDirectory() as output_dir:
            store = skip_news_web.FavouriteStore(Path(output_dir) / "favourites.json")
            track = {"name": "Track", "artist": {"name": "Artist"}}
            store.add({**track, "programme": "First show"})
            store.add({**track, "programme": "Current show"})

            favourites = store.list()["tracks"]
            self.assertEqual(len(favourites), 1)
            self.assertEqual(favourites[0]["programme"], "Current show")


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

    def test_normalizes_fip_track_programme_and_artwork(self):
        result = skip_news_web.fip_now_playing_from_payload(
            {
                "prev": [
                    {
                        "firstLine": "FIP",
                        "cover": "34e98566-058b-428f-a39e-d74bdef1cf77",
                    }
                ],
                "now": {
                    "firstLine": "Club Jazzafip",
                    "secondLine": "Anna Erhard • Spa",
                    "cover": "39558008-e0cb-40cd-978c-604a63eff4c2",
                    "startTime": 1789383015,
                    "endTime": 1789383185,
                },
                "next": [],
                "delayToRefresh": 150000,
            }
        )

        self.assertTrue(result["available"])
        self.assertTrue(result["now_playing"])
        self.assertEqual(result["source"], "FIP")
        self.assertEqual(result["artist"], "Anna Erhard")
        self.assertEqual(result["title"], "Spa")
        self.assertEqual(result["programme"], "Club Jazzafip")
        self.assertEqual(result["show"]["title"], "Club Jazzafip")
        self.assertEqual(
            result["image_url"],
            "https://www.radiofrance.fr/pikapi/images/"
            "39558008-e0cb-40cd-978c-604a63eff4c2/640",
        )
        self.assertEqual(
            result["programme_image_url"],
            "https://www.radiofrance.fr/pikapi/images/"
            "34e98566-058b-428f-a39e-d74bdef1cf77/640",
        )

    def test_rejects_untrusted_fip_artwork(self):
        result = skip_news_web.fip_now_playing_from_payload(
            {
                "now": {
                    "firstLine": "FIP",
                    "secondLine": "Artist • Track",
                    "cover": "https://example.com/untrusted.jpg",
                }
            }
        )

        self.assertIsNone(result["image_url"])


class NewsStatusTests(unittest.TestCase):
    def test_reads_fresh_news_state(self):
        with tempfile.TemporaryDirectory() as output_dir:
            path = Path(output_dir) / "news-status.json"
            path.write_text(
                '{"news_active":true,"transitioning":true,"sample":48000,'
                '"updated_at_unix":100}',
                encoding="utf-8",
            )
            service = skip_news_web.NewsStatusService(path, max_age_seconds=10)

            self.assertEqual(
                service.get(now=105),
                {
                    "news_active": True,
                    "transitioning": True,
                    "sample": 48000,
                    "updated_at_unix": 100,
                    "fresh": True,
                },
            )

    def test_treats_missing_or_stale_state_as_off(self):
        with tempfile.TemporaryDirectory() as output_dir:
            path = Path(output_dir) / "news-status.json"
            service = skip_news_web.NewsStatusService(path, max_age_seconds=10)
            self.assertFalse(service.get(now=105)["news_active"])

            path.write_text(
                '{"news_active":true,"sample":1,"updated_at_unix":100}',
                encoding="utf-8",
            )
            self.assertFalse(service.get(now=111)["news_active"])


class NewsControlTests(unittest.TestCase):
    def test_sends_manual_news_events_to_fifo(self):
        with tempfile.TemporaryDirectory() as output_dir:
            path = Path(output_dir) / "news-control.fifo"
            os.mkfifo(path)
            reader = os.open(path, os.O_RDONLY | os.O_NONBLOCK)
            try:
                service = skip_news_web.NewsControlService(path)
                self.assertEqual(
                    service.set_fip(True),
                    {"fip_enabled": True, "event": "news_on"},
                )
                self.assertEqual(
                    os.read(reader, 1024),
                    b"NEWS_EVENT news_on sample=0 delay_ms=0\n",
                )
                self.assertEqual(
                    service.set_fip(False),
                    {"fip_enabled": False, "event": "news_off"},
                )
                self.assertEqual(
                    os.read(reader, 1024),
                    b"NEWS_EVENT news_off sample=0 delay_ms=0\n",
                )
            finally:
                os.close(reader)

    def test_rejects_invalid_setting_or_missing_fifo(self):
        with tempfile.TemporaryDirectory() as output_dir:
            service = skip_news_web.NewsControlService(
                Path(output_dir) / "news-control.fifo"
            )
            with self.assertRaises(ValueError):
                service.set_fip("yes")
            with self.assertRaises(OSError):
                service.set_fip(True)


class ScheduleTests(unittest.TestCase):
    OUTPUT = (
        "SKIPPER|||m00318j6|||Nick Grimshaw|||Guests, chat and really good songs|||"
        "2026-09-14T09:00:00+00:00|||10800|||"
        "https://ichef.bbci.co.uk/images/ic/192xn/p0m53m3f.jpg|||"
        "https://www.bbc.co.uk/programmes/m00318j6\n"
        "SKIPPER|||m00318j8|||Lauren Laverne|||Words from Gemma Cairney|||"
        "2026-09-14T12:00:00+00:00|||10800|||"
        "https://ichef.bbci.co.uk/images/ic/192xn/p0m53ld7.jpg|||"
        "https://www.bbc.co.uk/programmes/m00318j8\n"
    )

    def test_selects_current_programme_from_get_iplayer_schedule(self):
        programmes = skip_news_web.get_iplayer_schedule_from_output(self.OUTPUT)
        current = skip_news_web.current_schedule_programme(
            programmes,
            at=skip_news_web.datetime.fromisoformat("2026-09-14T10:30:00+00:00"),
        )

        self.assertEqual(current["pid"], "m00318j8")
        self.assertEqual(current["title"], "Lauren Laverne")
        self.assertEqual(current["subtitle"], "Words from Gemma Cairney")
        self.assertEqual(
            current["image_url"],
            "https://ichef.bbci.co.uk/images/ic/640x640/p0m53ld7.jpg",
        )
        self.assertEqual(current["start_time"], "2026-09-14T09:00:00+00:00")
        self.assertEqual(current["end_time"], "2026-09-14T12:00:00+00:00")
        self.assertNotIn("start_timestamp", current)

    def test_schedule_service_refreshes_once_per_hour(self):
        with tempfile.TemporaryDirectory() as output_dir:
            service = skip_news_web.BBCScheduleService(output_dir)
            calls = []

            def refresh():
                calls.append(True)
                return skip_news_web.get_iplayer_schedule_from_output(self.OUTPUT)

            service._refresh = refresh
            at = skip_news_web.datetime.fromisoformat("2026-09-14T10:30:00+00:00")
            self.assertTrue(service.get(at=at)["available"])
            self.assertTrue(service.get(at=at)["available"])
            self.assertEqual(len(calls), 1)
            service.cached_at -= service.cache_seconds + 1
            self.assertTrue(service.get(at=at)["available"])
            self.assertEqual(len(calls), 2)

    def test_bridges_a_short_gap_in_get_iplayer_schedule(self):
        programmes = skip_news_web.get_iplayer_schedule_from_output(self.OUTPUT)
        current = skip_news_web.current_schedule_programme(
            programmes,
            at=skip_news_web.datetime.fromisoformat("2026-09-14T12:30:00+00:00"),
        )

        self.assertEqual(current["title"], "Lauren Laverne")
        self.assertTrue(current["schedule_inferred"])


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
cat > "$out/$prefix.tracks.txt" <<'TRACKS'
Example Show
Example episode
2026-09-14
https://www.bbc.co.uk/sounds/play/m0030yw7
Music
--------
00:00:10
Example Artist
Example Track
Duration: 00:03:00
--------
TRACKS
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
            self.assertTrue((output_dir / "m0030yw7_newsskip.tracks.txt").is_file())
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
                """sample rate = 10
timeline: input_samples=100 output_samples=100 discarded_samples=0
timeline: input_samples=150 output_samples=100 discarded_samples=50
timeline: input_samples=250 output_samples=200 discarded_samples=50
""",
                encoding="utf-8",
            )
            (output_dir / "m0030yw7_newsskip.tracks.txt").write_text(
                TracklistTests.TRACKLIST, encoding="utf-8"
            )

            store = skip_news_web.JobStore(ROOT / "get_iplayer_skip_news.sh", output_dir)
            jobs = store.list_jobs()

            self.assertEqual(jobs["active"], [])
            self.assertEqual(len(jobs["history"]), 1)
            self.assertEqual(jobs["history"][0]["pid"], "m0030yw7")
            self.assertEqual(jobs["history"][0]["filename"], result.name)
            self.assertEqual(jobs["history"][0]["file_size"], len(b"previous audio"))
            self.assertEqual(jobs["history"][0]["media_url"], "/media/" + result.name)
            self.assertEqual(jobs["history"][0]["tracks"][0]["start_seconds"], 11)
            self.assertEqual(jobs["history"][0]["tracks"][0]["programme"], "Example Show")
            self.assertEqual(jobs["history"][0]["tracks"][0]["presenter"], "Example episode")

    def test_removes_processed_audio_and_log(self):
        with tempfile.TemporaryDirectory() as temp_root:
            output_dir = Path(temp_root) / "output"
            output_dir.mkdir()
            result = output_dir / "m0030yw7_newsskip.m4a"
            log = output_dir / "m0030yw7_newsskip.log"
            artwork = output_dir / "m0030yw7_newsskip.artwork.jpg"
            tracklist = output_dir / "m0030yw7_newsskip.tracks.txt"
            result.write_bytes(b"audio")
            log.write_text("log", encoding="utf-8")
            artwork.write_bytes(b"artwork")
            tracklist.write_text(TracklistTests.TRACKLIST, encoding="utf-8")
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
            self.assertFalse(tracklist.exists())
            self.assertEqual(store.list_jobs()["history"], [])

    def test_removes_original_audio_and_all_associated_sidecars(self):
        with tempfile.TemporaryDirectory() as temp_root:
            output_dir = Path(temp_root) / "output"
            output_dir.mkdir()
            processed = output_dir / "m0030yw7_newsskip.m4a"
            original = output_dir / "m0030yw7.m4a"
            other_version = output_dir / "m0030yw7_newsskip_2.m4a"
            unrelated = output_dir / "unrelated.m4a"
            associated = [
                processed,
                output_dir / "m0030yw7_newsskip.log",
                output_dir / "m0030yw7_newsskip.artwork.jpg",
                output_dir / "m0030yw7_newsskip.tracks.txt",
                original,
                output_dir / "m0030yw7.log",
                output_dir / "m0030yw7.artwork.jpg",
                output_dir / "m0030yw7.tracks.txt",
            ]
            for path in associated:
                path.write_bytes(b"associated")
            other_version.write_bytes(b"keep another processed version")
            unrelated.write_bytes(b"keep unrelated")

            store = skip_news_web.JobStore(ROOT / "get_iplayer_skip_news.sh", output_dir)
            job = next(
                item
                for item in store.list_jobs()["history"]
                if item["filename"] == processed.name
            )

            store.remove(job["id"])

            self.assertTrue(all(not path.exists() for path in associated))
            self.assertTrue(other_version.exists())
            self.assertTrue(unrelated.exists())
            remaining = {
                item["filename"] for item in store.list_jobs()["history"]
            }
            self.assertEqual(remaining, {other_version.name, unrelated.name})

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
        self.assertIn('id="live-now-playing-title"', html)
        self.assertIn('id="live-now-playing-favourite"', html)
        self.assertIn('id="live-programme-image"', html)
        self.assertIn('id="site-icon"', html)
        self.assertIn("updateSiteIcon(currentLiveProgramme?.image_url)", app)
        self.assertIn('id="fip-toggle"', html)
        self.assertIn('role="switch"', html)
        self.assertIn('fetch("/api/fip-toggle"', app)
        self.assertIn("fipToggleCooldownUntil = Date.now() + 30000", app)
        self.assertIn("track.news?.transitioning === true", app)
        self.assertIn("fipToggle.disabled = true", app)
        self.assertIn('.fip-toggle[aria-checked="true"]', styles)
        self.assertIn(".fip-toggle { min-height: 24px; }", styles)
        self.assertIn('fetch("/api/now-playing"', app)
        self.assertIn("setInterval(loadNowPlaying, 5000)", app)
        self.assertIn("display_delay_seconds", app)
        self.assertIn('"mediaSession" in navigator', app)
        self.assertIn("new MediaMetadata", app)
        self.assertIn("navigator.mediaSession.playbackState", app)
        self.assertIn("navigator.mediaSession.setPositionState", app)
        self.assertIn("activeMediaPlayer === radioPlayer", app)
        self.assertIn("renderHeaderHistory(entry", app)
        self.assertIn("renderHeaderProgramme(currentLiveProgramme)", app)
        self.assertIn('stationName(track) === "FIP"', app)
        self.assertIn('classList.toggle("is-fip-live", isFip)', app)
        self.assertIn('id="live-description"', html)
        self.assertIn(".live-section.is-fip", styles)
        self.assertNotIn("renderLiveProgramme(currentLiveProgramme)", app)
        self.assertIn("radioStatus.textContent = liveProgrammeStatus()", app)
        self.assertIn("grid-template-columns: 190px minmax(0, 1fr)", styles)
        self.assertIn("object-fit: contain", styles)
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

    def test_fip_is_event_crossfaded_and_status_is_shared_with_web(self):
        script = (ROOT / "radio6music_noNews_hls.sh").read_text(encoding="utf-8")
        compose = (ROOT / "docker-compose.yml").read_text(encoding="utf-8")

        self.assertIn('"$NEWS_IDENTIFIER" -e -t -x', script)
        self.assertIn("volume@bbc=1", script)
        self.assertIn("volume@fip=0", script)
        self.assertIn('"$NEWS_MIXER_CONTROL" "$NEWS_STATUS_FILE"', script)
        self.assertIn('NEWS_EVENT_PIPE="$NEWS_CONTROL_PIPE"', script)
        self.assertNotIn("sidechaincompress", script)
        self.assertIn("NEWS_STATUS_FILE: /srv/hls/news-status.json", compose)
        self.assertIn("NEWS_CONTROL_PIPE: /srv/hls/news-control.fifo", compose)
        self.assertIn("- hls_data:/srv/hls:ro", compose)

    def test_favourites_use_a_shared_lastfm_compatible_store(self):
        html = (ROOT / "web/index.html").read_text(encoding="utf-8")
        app = (ROOT / "web/app.js").read_text(encoding="utf-8")
        favourites = (ROOT / "web/favourites.js").read_text(encoding="utf-8")
        favourites_page = (ROOT / "web/favourites.html").read_text(encoding="utf-8")

        self.assertIn('href="#favourites">Favourites</a>', html)
        self.assertIn('id="favourites"', html)
        self.assertIn('id="header-radio-toggle"', html)
        self.assertIn('id="now-playing-favourite"', html)
        self.assertIn('src="/favourites.js"', html)
        self.assertIn('src="/favourites-page.js"', html)
        self.assertIn("skipper.favouriteTracks.v1", favourites)
        self.assertIn('const API_URL = "/api/favourites"', favourites)
        self.assertIn("window.SkipperFavourites.load()", app)
        self.assertIn("https://www.google.com/search?q=", favourites)
        self.assertIn("programme: clean(track?.programme)", favourites)
        self.assertIn("presenter: clean(track?.presenter)", favourites)
        self.assertIn('id="favourites-list"', favourites_page)


if __name__ == "__main__":
    unittest.main()
