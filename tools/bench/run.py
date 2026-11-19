"""Run fresh-process A/B benchmarks with the WinMM build of Stellaris Engine Fixes."""

import argparse
import configparser
from contextlib import contextmanager
import csv
import ctypes
from ctypes import wintypes
from dataclasses import dataclass
import hashlib
from itertools import product
import json
import math
import os
from pathlib import Path
import re
import shutil
import statistics
import subprocess
import sys
import tempfile
import time
from zipfile import BadZipFile, ZipFile


HERE = Path(__file__).resolve().parent
PROJECT = HERE.parents[1]
BUILD = PROJECT / "build"
INI = "stellaris-engine-fixes.ini"
LOG = "stellaris-engine-fixes.log"
GAME_FILES = ("winmm.dll", INI, "userdir.txt", LOG)
FIXTURES = {
    "earlyGame": (3, 90),
    "midGame": (3, 90),
    "lateGame": (3, 90),
    "bigBattle": (1, 10),
}


@dataclass(frozen=True)
class Case:
    section: str
    key: str
    a: int = 0
    b: int = 1
    component: str = ""
    on_message: str = ""
    common: tuple = ()

    @property
    def name(self):
        return f"{self.section}.{self.key}" if self.section else self.key

    @property
    def primary_metric(self):
        if self.component in ("file_lookups", "token_strings", "deferred_refs", "modifier_docs"):
            return "load_ms"
        return "ms_per_day"


CASES = (
    Case("speed", "in_tick_frame_ms", b=33),
    Case("speed", "batch_ticks"),
    Case("speed", "fast_intel_lookup", component="fleet_intel", on_message="hooks installed"),
    Case("speed", "skip_empty_combat_entries", component="combat_map", on_message="hook installed"),
    Case("speed", "mega_afford_cache", component="mega_afford", on_message="hooks installed"),
    Case("speed", "fast_income_target", component="ai_income_targets", on_message="rounds after the first are counted",
         common=(("numeric_fixes", "income_target_arithmetic", 0),)),
    Case("speed", "fast_file_lookups", component="file_lookups", on_message="import hooks installed"),
    Case("speed", "fast_token_rebuild", component="token_strings", on_message="hook installed"),
    Case("speed", "fast_deferred_unregister", component="deferred_refs", on_message="hooks installed"),
    Case("speed", "skip_modifier_docs", component="modifier_docs", on_message="modifiers.log stays empty"),
    Case("mimalloc", "enabled"),
    Case("mimalloc", "reserve_huge_os_pages"),
    Case("mimalloc", "allow_large_os_pages", common=(("mimalloc", "arena_eager_commit", 1),)),
    Case("mimalloc", "arena_eager_commit", common=(("mimalloc", "allow_large_os_pages", 0),)),
    Case("mimalloc", "purge_delay", b=10000),
)
ALL = Case("", "all")
CASE_BY_NAME = {case.name: case for case in (*CASES, ALL)}
FIELDS = re.compile(r"(\w+)=([^\s]+)")
RECORD = re.compile(r"^\S+ (\w+)\s+(\w+) \| (.*)$", re.MULTILINE)


class BenchError(Exception):
    pass


class Unavailable(BenchError):
    pass


class Cancelled(BenchError):
    pass


def read_save(path):
    with ZipFile(path) as save:
        if "gamestate" not in save.namelist():
            raise BenchError(f"No gamestate in {path}")
        with save.open("meta") as meta:
            text = meta.read(65536).decode("utf-8-sig", errors="replace")
    date = re.search(r'(?m)^\s*date\s*=\s*"?(\d+)\.(\d+)\.(\d+)"?', text)
    if not date:
        raise BenchError(f"No save date in {path}")
    if re.search(r'(?im)^\s*ironman\s*=\s*"?(?:yes|true|1)\b', text):
        raise BenchError(f"Use a non-Ironman fixture for {path.stem}")
    year, month, day = map(int, date.groups())
    if not (0 <= year <= 9999 and 1 <= month <= 12 and 1 <= day <= 30):
        raise BenchError(f"Invalid save date in {path}")
    hours = 43800000 + 24 * (year * 360 + (month - 1) * 30 + day - 1)
    return f"{year:04}.{month:02}.{day:02}", hours


def format_date(hours):
    days = (hours - 43800000) // 24
    year, day = divmod(days, 360)
    month, day = divmod(day, 30)
    return f"{year:04}.{month + 1:02}.{day + 1:02}"


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def config_for(ini, case, value, hours, warmup, days, huge_pages_gib=1):
    cfg = configparser.ConfigParser(interpolation=None)
    with ini.open(encoding="utf-8-sig") as source:
        cfg.read_file(source)
    for key in cfg["speed"]:
        if key.endswith("_verify"):
            cfg["speed"][key] = "0"
    # SEF script storage lives by the DLL, outside the game's redirected user directory.
    cfg["script_storage"]["enabled"] = "0"
    if case == ALL:
        cfg["numeric_fixes"]["income_target_arithmetic"] = "0"
        for setting in CASES:
            enabled = huge_pages_gib if setting.key == "reserve_huge_os_pages" else setting.b
            cfg[setting.section][setting.key] = str(enabled if value else setting.a)
    elif case.section == "mimalloc" and case.key != "enabled":
        cfg["mimalloc"]["enabled"] = "1"
        cfg["mimalloc"]["reserve_huge_os_pages"] = "0"
    for section, key, common_value in case.common:
        cfg[section][key] = str(common_value)
    if case != ALL:
        cfg[case.section][case.key] = str(value)
    cfg["benchmark"] = {"days": str(days), "warmup_days": str(warmup), "save_date_hours": str(hours)}
    return cfg


def atomic_copy(source, target):
    # Replace in the target directory, even if the source is on a different drive.
    with tempfile.NamedTemporaryFile(dir=target.parent, delete=False) as stream:
        temp = Path(stream.name)
    try:
        shutil.copyfile(source, temp)
        os.replace(temp, target)
    finally:
        temp.unlink(missing_ok=True)


def require_idle():
    command = ["tasklist", "/FI", "IMAGENAME eq stellaris.exe", "/FO", "CSV", "/NH"]
    result = subprocess.run(command, capture_output=True, check=True, text=True, errors="replace")
    if any(row and row[0].casefold() == "stellaris.exe" for row in csv.reader(result.stdout.splitlines())):
        raise BenchError("Close Stellaris before running the benchmarks.")


@contextmanager
def benchmark_install(game, output, dll):
    require_idle()
    lock = game / ".sef-bench.lock"
    backup = output / "original-game-files"
    try:
        with lock.open("x", encoding="utf-8") as stream:
            stream.write(str(backup))
    except FileExistsError as exc:
        raise BenchError(f"A benchmark is active or interrupted. Check {lock}.") from exc
    restore_needed = False
    restored = False
    try:
        backup.mkdir()
        originals = {}
        for name in GAME_FILES:
            source = game / name
            originals[name] = source.exists()
            if source.exists():
                shutil.copyfile(source, backup / name)
        (backup / "files.json").write_text(json.dumps(originals, indent=2), encoding="utf-8")
        restore_needed = True
        atomic_copy(dll, game / "winmm.dll")
        yield
    finally:
        if restore_needed:
            try:
                require_idle()
                for name, existed in originals.items():
                    target = game / name
                    if existed:
                        atomic_copy(backup / name, target)
                    else:
                        target.unlink(missing_ok=True)
                restored = True
            except (OSError, BenchError, subprocess.CalledProcessError) as exc:
                raise BenchError(f"Game files need restoration from {backup}. {exc}") from exc
        if not restore_needed or restored:
            lock.unlink()


def stage_profile(profile, save, date, template):
    if template:
        for name in ("settings.txt", "pdx_settings.txt", "dlc_load.json", "message_settings.txt", "alert_settings.txt"):
            if (template / name).is_file():
                shutil.copyfile(template / name, profile / name)
        for descriptor in (template / "mod").glob("*.mod"):
            (profile / "mod").mkdir(exist_ok=True)
            dest = profile / "mod" / descriptor.name
            text = descriptor.read_text(encoding="utf-8-sig")

            def absolute_path(match):
                path = Path(match[2])
                return f'{match[1]}"{(template / path).resolve().as_posix()}"'

            text = re.sub(r'(?m)^(\s*(?:path|archive)\s*=\s*)"([^"]+)"', absolute_path, text)
            dest.write_text(text, encoding="utf-8")
    if not (profile / "dlc_load.json").exists():
        (profile / "dlc_load.json").write_text('{"disabled_dlcs":[],"enabled_mods":[]}', encoding="utf-8")
    target = profile / "save games" / "SEF Benchmark" / save.name
    target.parent.mkdir(parents=True)
    shutil.copyfile(save, target)
    title = target.relative_to(profile).with_suffix("").as_posix()
    (profile / "continue_game.json").write_text(
        json.dumps({"title": title, "desc": save.stem, "date": date}), encoding="utf-8"
    )


def request_close(pid):
    user32 = ctypes.WinDLL("user32", use_last_error=True)
    callback_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    user32.EnumWindows.argtypes = (callback_type, wintypes.LPARAM)
    user32.GetWindowThreadProcessId.argtypes = (wintypes.HWND, ctypes.POINTER(wintypes.DWORD))
    user32.IsWindowVisible.argtypes = (wintypes.HWND,)
    user32.PostMessageW.argtypes = (wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM)

    @callback_type
    def visit(window, _):
        owner = wintypes.DWORD()
        user32.GetWindowThreadProcessId(window, ctypes.byref(owner))
        if owner.value == pid and user32.IsWindowVisible(window):
            user32.PostMessageW(window, 0x0010, 0, 0)  # WM_CLOSE
        return True

    user32.EnumWindows(visit, 0)


def close_game(process, timeout):
    if process.poll() is None:
        request_close(process.pid)
    try:
        return {"method": "exit", "exit_code": process.wait(timeout)}
    except subprocess.TimeoutExpired:
        # Only the process this run started, after its result was flushed or the run timed out.
        process.kill()
        return {"method": "terminate", "exit_code": process.wait(15)}


def poll_result(process, path, load_timeout, run_timeout, cancel):
    deadline = time.monotonic() + load_timeout
    loaded = False
    offset = 0
    while True:
        if cancel.exists():
            raise Cancelled("Benchmark cancelled")
        try:
            with path.open("rb") as stream:
                stream.seek(offset)
                chunk = stream.read()
        except (FileNotFoundError, PermissionError):
            chunk = b""
        complete_lines = chunk.rfind(b"\n") + 1
        offset += complete_lines
        tail = chunk[:complete_lines].decode("utf-8", errors="replace")
        for level, component, message in RECORD.findall(tail):
            if component == "sef_auto_bench" and level == "ERROR":
                raise BenchError(message)
            if component == "sef_auto_bench" and message.startswith("ready_tick_ms=") and not loaded:
                deadline = time.monotonic() + run_timeout
                loaded = True
            if component == "sef_auto_bench" and message.startswith("complete=1"):
                return path.read_text(encoding="utf-8", errors="replace")
        if process.poll() is not None:
            raise BenchError(f"Game exited with code {process.returncode} before the benchmark finished")
        if time.monotonic() > deadline:
            raise BenchError("Benchmark timed out" if loaded else "Save did not become ready before the load timeout")
        time.sleep(0.2)


def extract_result(log, case, value, launch_tick, cfg, hours, warmup, days):
    messages = {}
    for level, component, message in RECORD.findall(log):
        messages.setdefault(component, []).append((level, message))

    def fields(component, prefix):
        for _, message in reversed(messages.get(component, [])):
            if message.startswith(prefix):
                return dict(FIELDS.findall(message))
        raise BenchError(f"Missing {component} result ({prefix})")

    def has(component, fragment):
        return any(fragment in message for _, message in messages.get(component, []))

    bench = fields("sef_bench", "wall_s=")
    ready = fields("sef_auto_bench", "ready_tick_ms=")
    memory = fields("sef_auto_bench", "complete=1")
    if (ready["save_date"] != format_date(hours) or bench["first_date"] != format_date(hours + 24 * warmup)
            or bench["last_date"] != format_date(hours + 24 * (warmup + days)) or int(bench["days"]) != days
            or bench["speed"] != "5" or bench["speed_changed"] != "0" or bench["smoothing_changed"] != "0"
            or float(bench["paused_s"]) > 0):
        raise BenchError("Unexpected date, speed, pause or frame smoothing in the measurement")
    if (int(bench["in_tick_frame_ms"]) != cfg.getint("speed", "in_tick_frame_ms")
            or int(bench["batch_ticks"]) != cfg.getint("speed", "batch_ticks")):
        raise BenchError("Game did not apply the requested pacing settings")
    for setting in CASES if case == ALL else (case,):
        if setting.component:
            if any(level == "ERROR" for level, _ in messages.get(setting.component, [])):
                raise BenchError(f"{setting.component} reported an error")
            off_message = f"{setting.key}=0"
            enabled = cfg.getint(setting.section, setting.key)
            if not has(setting.component, setting.on_message if enabled else off_message):
                raise BenchError(f"Could not verify {setting.name}={enabled} in the game log")
    has_heap = has("allocator", "replaces the game's heap")
    if has_heap != cfg.getboolean("mimalloc", "enabled"):
        raise BenchError("Game did not apply the requested allocator")

    load_ms = int(ready["ready_tick_ms"]) - launch_tick
    if load_ms < 0:
        raise BenchError("Readiness timestamp predates this launch")
    result = {**bench, "load_ms": load_ms}
    for key in ("wall_s", "active_s", "paused_s", "ms_per_day", "days_per_s", "simulation_s",
                "in_tick_frame_s", "between_tick_s", "longest_tick_ms"):
        result[key] = float(bench[key])
    for key in ("peak_working_set_bytes", "peak_commit_bytes"):
        result[key] = int(memory[key])
    if (case == ALL or case.key == "reserve_huge_os_pages") and value:
        requested = cfg.getint("mimalloc", "reserve_huge_os_pages")
        reservation = fields("allocator", "huge_pages_requested_gib=")
        got = int(reservation["huge_pages_reserved_gib"])
        if got != requested and case != ALL:
            raise Unavailable(f"Requested {requested} GiB of huge pages, reserved {got} GiB")
        result["huge_pages_requested_gib"] = requested
        result["huge_pages_reserved_gib"] = got
    if (case == ALL or case.key == "allow_large_os_pages") and value and has("mimalloc", "cannot enable large OS page support"):
        if case != ALL:
            raise Unavailable("Windows could not enable large-page support")
        result["large_page_support_unavailable"] = True
    return result


def run_trial(args, game, folder, save, date, hours, warmup, days, case, value):
    folder.mkdir()
    cfg = config_for(args.ini, case, value, hours, warmup, days, args.huge_pages_gib)
    with (folder / INI).open("w", encoding="ascii") as stream:
        cfg.write(stream, space_around_delimiters=False)
    env = {k: v for k, v in os.environ.items() if not k.upper().startswith("MIMALLOC_")}
    tick = ctypes.WinDLL("kernel32").GetTickCount64
    tick.restype = ctypes.c_uint64
    live_log = game / LOG
    with tempfile.TemporaryDirectory(prefix="profile-", dir=folder) as directory:
        profile = Path(directory)
        if len(str(profile / "logs" / "system.log")) >= 260 or len(profile.as_posix().encode("utf-8")) >= 512:
            raise BenchError("Benchmark profile path is too long for the game. Use a shorter --output path.")
        env["SEF_BENCH_USERDIR"] = str(profile)
        stage_profile(profile, save, date, args.profile_template)
        redirect = folder / "userdir.txt"
        redirect.write_bytes(profile.as_posix().encode("utf-8"))
        atomic_copy(redirect, game / "userdir.txt")
        atomic_copy(folder / INI, game / INI)
        live_log.unlink(missing_ok=True)
        launch_tick = tick()
        process = subprocess.Popen([str(game / "stellaris.exe"), "-dx11", "--continuelastsave", "-loadrandomseed"],
                                   cwd=game, env=env)
        try:
            log = poll_result(process, live_log, args.load_timeout, args.run_timeout, folder.parent / "CANCEL")
            result = extract_result(log, case, value, launch_tick, cfg, hours, warmup, days)
        finally:
            shutdown = close_game(process, args.quit_timeout)
            (folder / "shutdown.json").write_text(json.dumps(shutdown), encoding="utf-8")
            if live_log.is_file():
                shutil.copyfile(live_log, folder / LOG)
            if (profile / "logs" / "error.log").is_file():
                shutil.copyfile(profile / "logs" / "error.log", folder / "game-errors.log")
        if shutdown["method"] == "exit" and shutdown["exit_code"] != 0:
            raise BenchError(f"Game exited with code {shutdown['exit_code']}")
        return result


def summarize(trials, cases):
    metrics = ("load_ms", "ms_per_day", "simulation_s", "in_tick_frame_s", "between_tick_s",
               "peak_working_set_bytes", "peak_commit_bytes")
    grouped = {}
    for trial in trials:
        if trial["status"] == "ok":
            key = trial["case"], trial["save"], trial["pair"]
            grouped.setdefault(key, {})[trial["arm"]] = trial["metrics"]
    comparisons = []
    for case, fixture in dict.fromkeys((t["case"], t["save"]) for t in trials):
        pairs = [pair for (c, s, _), pair in grouped.items() if (c, s) == (case, fixture) and len(pair) == 2]
        effects = {}
        for metric in metrics:
            ratios = [pair["B"][metric] / pair["A"][metric] for pair in pairs
                      if pair["A"][metric] > 0 and pair["B"][metric] > 0]
            if ratios:
                effects[metric] = 100 * math.expm1(statistics.mean(map(math.log, ratios)))
        comparisons.append({"case": case, "save": fixture, "pairs": len(pairs),
                            "primary_metric": cases[case].primary_metric, "change_pct": effects})
    return comparisons


def run_suite(args, cases, fixtures):
    game_path = args.game_dir or os.environ.get("STELLARIS_DIR")
    if not game_path:
        raise BenchError("Pass --game-dir or set STELLARIS_DIR to the installed game folder")
    game = Path(game_path).resolve()
    if not (game / "stellaris.exe").is_file():
        raise BenchError(f"No stellaris.exe in {game}")
    if not args.dll.is_file() or not args.ini.is_file():
        raise BenchError("Build stellaris-engine-fixes first or pass --dll and --ini")
    if args.profile_template and not args.profile_template.is_dir():
        raise BenchError(f"No profile template at {args.profile_template}")
    inputs = {name: HERE / "saves" / f"{name}.sav" for name in fixtures}
    missing = [str(path) for path in inputs.values() if not path.is_file()]
    if missing:
        raise BenchError("Missing benchmark saves:\n  " + "\n  ".join(missing))
    require_idle()
    args.output.mkdir(parents=True, exist_ok=True)
    output = Path(tempfile.mkdtemp(prefix="bench-", dir=args.output.resolve()))
    print(f"Benchmark output: {output}", flush=True)
    # Snapshot the baseline and fixtures once for all launches in this suite.
    baseline = output / "baseline.ini"
    shutil.copyfile(args.ini, baseline)
    args.ini = baseline
    snapshots = output / "saves"
    snapshots.mkdir()
    dates = {}
    for name, source in inputs.items():
        target = snapshots / source.name
        shutil.copyfile(source, target)
        inputs[name] = target
        dates[name] = read_save(target)
    manifest = {
        "game_exe_sha256": sha256(game / "stellaris.exe"),
        "winmm_dll_sha256": sha256(args.dll),
        "baseline_ini_sha256": sha256(baseline),
        "saves": {name: {"sha256": sha256(path), "date": dates[name][0]} for name, path in inputs.items()},
    }
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    trials = []
    with benchmark_install(game, output, args.dll):
        for case, name, pair in product(cases, fixtures, range(1, args.pairs + 1)):
            warmup, days = FIXTURES[name]
            warmup, days = args.warmup_days or warmup, args.days or days
            date, hours = dates[name]
            # Pair by save, alternating which configuration gets the first launch.
            for arm in "AB" if pair % 2 else "BA":
                require_idle()
                value = case.a if arm == "A" else case.b
                if arm == "B" and case.key == "reserve_huge_os_pages":
                    value = args.huge_pages_gib
                label = f"{case.name}-{name}-{pair:02d}{arm}"
                print(f"{label} ({case.key}={value})", flush=True)
                record = {"case": case.name, "save": name, "pair": pair, "arm": arm, "value": value}
                try:
                    record["metrics"] = run_trial(args, game, output / label, inputs[name], date, hours,
                                                  warmup, days, case, value)
                    record["status"] = "ok"
                    metrics = record["metrics"]
                    if case == ALL and arm == "B":
                        requested, reserved = metrics["huge_pages_requested_gib"], metrics["huge_pages_reserved_gib"]
                        if requested != reserved:
                            print(f"  fallback: reserved {reserved} of {requested} GiB of huge pages", flush=True)
                        if metrics.get("large_page_support_unavailable"):
                            print("  fallback: Windows could not enable large-page support", flush=True)
                except Cancelled:
                    raise
                except (BenchError, OSError, KeyError, ValueError, subprocess.SubprocessError) as exc:
                    record.update(status="unavailable" if isinstance(exc, Unavailable) else "error", error=str(exc))
                    print(f"  {record['status']}: {exc}", flush=True)
                trials.append(record)
                (output / "results.json").write_text(
                    json.dumps({"trials": trials, "comparisons": summarize(trials, CASE_BY_NAME)}, indent=2), encoding="utf-8"
                )
    for item in summarize(trials, CASE_BY_NAME):
        metrics = ("ms_per_day", "load_ms") if item["case"] == ALL.name else (item["primary_metric"],)
        changes = [f"{item['change_pct'][metric]:+.1f}% {metric}" for metric in metrics if metric in item["change_pct"]]
        if changes:
            print(f"{item['case']} {item['save']}: {', '.join(changes)}, {item['pairs']} pairs")
    return int(any(t["status"] == "error" for t in trials))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game-dir", type=Path, help="folder with stellaris.exe, or STELLARIS_DIR")
    parser.add_argument("--dll", type=Path, default=BUILD / "winmm" / "winmm.dll")
    parser.add_argument("--ini", type=Path, default=PROJECT / INI, help="baseline SEF configuration")
    parser.add_argument("--profile-template", type=Path, help="copy settings and mod descriptors from this game profile")
    parser.add_argument("--output", type=Path, default=BUILD / "benchmarks")
    parser.add_argument("--case", action="append", choices=CASE_BY_NAME, help="select a comparison (repeatable)")
    parser.add_argument("--save", action="append", choices=FIXTURES, help="select a fixture (repeatable)")
    parser.add_argument("--pairs", type=int, default=2, help="A/B pairs per save and setting")
    parser.add_argument("--warmup-days", type=int, help="override the warm-up days for every fixture")
    parser.add_argument("--days", type=int, help="override the measured days for every fixture")
    parser.add_argument("--huge-pages-gib", type=int, default=1, help="reservation size in the huge page case")
    parser.add_argument("--load-timeout", type=int, default=600)
    parser.add_argument("--run-timeout", type=int, default=1800)
    parser.add_argument("--quit-timeout", type=int, default=10)
    parser.add_argument("--plan", action="store_true", help="list cases and save availability without launching the game")
    args = parser.parse_args()
    if min(args.pairs, args.huge_pages_gib, args.load_timeout, args.run_timeout, args.quit_timeout) < 1:
        parser.error("pairs, huge page size and timeouts must be positive")
    if (args.days is not None and not 1 <= args.days <= 3600
            or args.warmup_days is not None and not 1 <= args.warmup_days <= 360):
        parser.error("days must be in 1..3600 and warm-up days in 1..360")
    cases = [CASE_BY_NAME[name] for name in dict.fromkeys(args.case)] if args.case else (*CASES, ALL)
    fixtures = tuple(dict.fromkeys(args.save)) if args.save else tuple(FIXTURES)
    if args.plan:
        for case in cases:
            if case == ALL:
                print("all: all settings off vs all settings on (ms_per_day and load_ms)")
                continue
            b = args.huge_pages_gib if case.key == "reserve_huge_os_pages" else case.b
            print(f"{case.name:33} A={case.a:<5} B={b:<5} {case.primary_metric}")
        for fixture in fixtures:
            warmup, days = FIXTURES[fixture]
            path = HERE / "saves" / f"{fixture}.sav"
            print(f"{fixture}: {args.warmup_days or warmup} days warm-up, {args.days or days} measured"
                  f"{'' if path.is_file() else ' (save pending)'}")
        print(f"{len(cases) * len(fixtures) * args.pairs * 2} fresh launches")
        return 0
    if os.name != "nt":
        parser.error("Live benchmarking requires Windows")
    try:
        return run_suite(args, cases, fixtures)
    except (BenchError, OSError, ValueError, KeyError, BadZipFile, configparser.Error, subprocess.SubprocessError) as exc:
        parser.exit(1, f"Benchmark failed: {exc}\n")


if __name__ == "__main__":
    sys.exit(main())
