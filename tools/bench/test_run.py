"""Offline checks for the A/B runner. Run with python -m unittest discover -s tools/bench."""

import configparser
from contextlib import redirect_stdout
import io
import json
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch
from zipfile import ZipFile

import run


class BenchRunnerTests(unittest.TestCase):
    def test_cases_cover_shipped_performance_settings(self):
        cfg = configparser.ConfigParser()
        cfg.read(run.PROJECT / run.INI, encoding="utf-8")
        expected = {
            f"{section}.{key}"
            for section in ("speed", "mimalloc")
            for key in cfg[section]
            if not key.endswith("_verify")
        }
        self.assertEqual(expected | {"all"}, set(run.CASE_BY_NAME))
        self.assertEqual(len(expected), len(run.CASES))

    def test_all_arms_set_each_performance_option(self):
        for arm in (0, 1):
            with self.subTest(arm=arm):
                cfg = run.config_for(run.PROJECT / run.INI, run.ALL, arm, 45000000, 3, 90, huge_pages_gib=2)
                for case in run.CASES:
                    b = 2 if case.key == "reserve_huge_os_pages" else case.b
                    self.assertEqual(cfg.getint(case.section, case.key), b if arm else case.a, case.name)
                self.assertEqual(cfg.getint("numeric_fixes", "income_target_arithmetic"), 0)

    def test_install_restores_files_after_failure(self):
        with tempfile.TemporaryDirectory() as directory, patch.object(run, "require_idle"):
            root = Path(directory)
            game, output = root / "game", root / "output"
            game.mkdir()
            output.mkdir()
            dll = root / "test.dll"
            dll.write_bytes(b"test")
            originals = {"winmm.dll": b"original dll", run.INI: b"original ini", run.LOG: b"original log"}
            for name, content in originals.items():
                (game / name).write_bytes(content)
            with self.assertRaisesRegex(run.BenchError, "timed out"), run.benchmark_install(game, output, dll):
                self.assertEqual((game / "winmm.dll").read_bytes(), b"test")
                (game / "userdir.txt").write_bytes(b"temporary profile")
                (game / run.INI).write_bytes(b"test settings")
                (game / run.LOG).unlink()
                raise run.BenchError("timed out")
            for name, content in originals.items():
                self.assertEqual((game / name).read_bytes(), content)
            self.assertFalse((game / "userdir.txt").exists())
            self.assertFalse((game / ".sef-bench.lock").exists())

    def test_two_pairs_stage_save_and_report_matched_intervals(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            game, saves = root / "game", root / "saves"
            game.mkdir()
            saves.mkdir()
            template = root / "template"
            template.mkdir()
            messages = 'EVENT_MESSAGE_TYPE={ pausegame=no }\n'
            (template / "message_settings.txt").write_text(messages, encoding="utf-8")
            (game / "stellaris.exe").write_bytes(b"fake executable")
            dll = root / "test.dll"
            dll.write_bytes(b"test dll")
            fixture = saves / "earlyGame.sav"
            with ZipFile(fixture, "w") as save:
                save.writestr("meta", 'date="2200.12.30"\n')
                save.writestr("gamestate", "fixture")
            date, hours = run.read_save(fixture)
            launches = []

            def fake_launch(command, *, cwd, env):
                self.assertEqual(cwd, game)
                self.assertEqual(command[1:], ["-dx11", "--continuelastsave", "-loadrandomseed"])
                profile = Path(env["SEF_BENCH_USERDIR"])
                self.assertEqual((game / "userdir.txt").read_text(), profile.as_posix())
                self.assertEqual((profile / "message_settings.txt").read_text(encoding="utf-8"), messages)
                copy = profile / "save games" / "SEF Benchmark" / fixture.name
                self.assertEqual(copy.read_bytes(), fixture.read_bytes())
                metadata = json.loads((profile / "continue_game.json").read_text())
                self.assertEqual(metadata["title"], "save games/SEF Benchmark/earlyGame")
                cfg = configparser.ConfigParser()
                cfg.read(game / run.INI, encoding="ascii")
                batch = cfg.getint("speed", "batch_ticks")
                launches.append(batch)
                self.assertEqual(cfg.getint("benchmark", "save_date_hours"), hours)
                self.assertEqual(cfg.getint("benchmark", "days"), 2)
                self.assertEqual(cfg.getint("script_storage", "enabled"), 0)
                fields = (
                    f"wall_s=0.2 active_s=0.2 paused_s=0 speed=5 speed_changed=0 smoothing_changed=0 "
                    f"in_tick_frame_ms=33 batch_ticks={batch} days=2 first_date=2201.01.01 last_date=2201.01.03 "
                    f"ms_per_day={90 if batch else 100} days_per_s=10 simulation_s=0.1 in_tick_frame_s=0.03 "
                    f"between_tick_s=0.07 longest_tick_ms=20"
                )
                records = (
                    ("allocator", "mimalloc replaces the game's heap"),
                    ("sef_auto_bench", f"ready_tick_ms=1200 save_date={date}"),
                    ("sef_bench", fields),
                    ("sef_auto_bench", "complete=1 peak_working_set_bytes=2000 peak_commit_bytes=1000"),
                )
                (game / run.LOG).write_text("".join(
                    f"2026-11-19T00:00:00.000Z STATS {component} | {text}\n" for component, text in records
                ), encoding="utf-8")
                return Mock(pid=456, poll=Mock(return_value=None), wait=Mock(return_value=0))

            args = SimpleNamespace(game_dir=game, dll=dll, ini=run.PROJECT / run.INI, profile_template=template,
                                   output=root / "output", pairs=2, warmup_days=1, days=2, huge_pages_gib=1,
                                   load_timeout=1, run_timeout=1, quit_timeout=1)
            with (
                patch.object(run, "HERE", root),
                patch.object(run, "require_idle"),
                patch.object(run, "request_close") as close,
                patch.object(run.ctypes, "WinDLL", create=True, return_value=SimpleNamespace(
                    GetTickCount64=Mock(return_value=1000))),
                patch.object(run.subprocess, "Popen", side_effect=fake_launch),
                redirect_stdout(io.StringIO()),
            ):
                self.assertEqual(run.run_suite(args, [run.CASE_BY_NAME["speed.batch_ticks"]], ["earlyGame"]), 0)

            self.assertEqual(launches, [0, 1, 1, 0])
            self.assertEqual(close.call_count, 4)
            result_path, = args.output.glob("*/results.json")
            report = json.loads(result_path.read_text())
            self.assertEqual(report["comparisons"][0]["pairs"], 2)
            self.assertAlmostEqual(report["comparisons"][0]["change_pct"]["ms_per_day"], -10)
            self.assertEqual(report["trials"][0]["metrics"]["load_ms"], 200)
            self.assertFalse((game / "winmm.dll").exists())
            self.assertFalse((game / "userdir.txt").exists())


if __name__ == "__main__":
    unittest.main()
