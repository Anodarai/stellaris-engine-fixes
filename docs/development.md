# Development and diagnostics

The [README](../README.md) covers installation and the features available to players, while
[Configuration](configuration.md) describes individual settings. This page covers the DLL's
loading behavior, logs, performance measurements, and offline validation.

The mod has had limited live testing, and multiplayer has not been tested at all. Passing an
offline check establishes the checks described below; it does not establish stability in a
running campaign, multiplayer synchronization, or achievement compatibility.

## How the DLL loads

Windows loads the local `winmm.dll` when `stellaris.exe` imports it. The proxy forwards the
Windows multimedia exports to the system DLL and installs its changes in the game's process
memory, leaving the original game executable on disk unchanged. It checks the host executable
name so that other programs loading the proxy do not receive the Stellaris hooks.

The memory allocator is installed during DLL initialization, before the game's C runtime starts
allocating memory. Other features locate game functions, globals, and structure offsets through
byte patterns and consistency checks, then install their hooks. This can allow a feature to
work across game updates that preserve the relevant code, but matching patterns alone cannot
establish that the surrounding game behavior is unchanged.

Fallbacks depend on where initialization fails. Individual installers usually leave their
feature off when required code cannot be located or hooked, while failures in shared
dependencies can prevent several features from loading. In particular, failure to locate the
shared string functions stops the remaining installation after allocator setup. Some features
also have partial fallbacks: script storage can retain its basic commands while save
persistence or recorded-script execution is unavailable. Read the component messages in the
log to see what actually loaded.

The entry point and installation order are in [src/dllmain.cpp](../src/dllmain.cpp), with
pattern definitions in [src/locate.h](../src/locate.h), allocator setup in
[src/allocator.h](../src/allocator.h), and proxy exports in
[src/exports_winmm.h](../src/exports_winmm.h).

## Log format

`stellaris-engine-fixes.log` is written beside the DLL and replaced at each launch. It can be
read while the game is running, and records are flushed as they are written. Keep a copy
before restarting when investigating a problem.

Every record contains a UTC timestamp with millisecond precision, a level, a component, and a
message. For example:

```text
2026-11-11T18:30:00.123Z INFO  fleets | hooks installed
2026-11-11T18:30:00.145Z WARN  file_lookups | GetFileAttributesExW isn't imported; only opens are answered
2026-11-11T18:30:00.167Z ERROR token_strings | failed to hook RebuildLookup; off
2026-11-11T18:30:30.124Z STATS fleets | sorts=410 compares=9300 keys=2050 avg_us=8.1 max_us=52.0 faulted=0
```

| Level | Meaning |
|---|---|
| `ERROR` | A failed operation, failed hook, or verification mismatch. |
| `WARN` | A partial fallback, configuration issue, or other condition needing attention. |
| `INFO` | Normal operation, including which features were enabled or disabled. |
| `DEBUG` | Hook details and diagnostic traces. |
| `STATS` | Startup timings, periodic counters, and benchmark results. |

For parsing, split at the first ` | ` and read the three whitespace-separated fields on the
left. The message escapes backslashes as `\\`, carriage returns as `\r`, newlines as `\n`,
tabs as `\t`, and other ASCII control bytes, including DEL, as `\xHH`. This keeps each record
on one physical line.

`STATS` messages use space-separated `key=value` fields. Numeric units appear in the keys where
applicable, such as `avg_us` and `wall_s`, and booleans use `0` or `1`. The writer is defined in
[src/logging.h](../src/logging.h).

## Measuring performance

The console command `sef_bench` measures the rate at which game days and ticks pass and how the
main thread spends its time. The command is available in a running single-player game outside
Ironman, provided its hooks installed successfully.

| Command | Behavior |
|---|---|
| `sef_bench` | Starts a 60-second measurement, or reports progress if a run is already active. |
| `sef_bench 120` | Starts a measurement lasting the requested number of wall-clock seconds; accepted durations are 5–3600 seconds. |
| `sef_bench stop` | Ends the current run early and prints its results. |
| `sef_bench show` | Prints the last completed result again. |
| `sef_bench combat` | Reports whether skipping empty combat entries is enabled. |
| `sef_bench combat off` / `sef_bench combat on` | Switches that optimization for comparison, if its hook was installed at startup. |

Results appear in the console and in a `STATS sef_bench` log record. They include days per
second, milliseconds per day, the longest tick, and time spent on simulation, frames rendered
inside ticks, and work between ticks. The measurement excludes paused time from its active-time
rates and reports changes to game speed or frame smoothing.

For a useful comparison, reload the same save and keep the playset, game speed, camera view,
resolution, and frame settings consistent. Compare several runs that include the same kinds
of daily and monthly work. Change the combat option between measurements, because switching
it during a run produces a result containing both behaviors. Most other settings require a
restart, as described in [Configuration](configuration.md).

### Previously reported results

These figures preserve the project's earlier measurements and describe particular workloads.
They are not a benchmark of every supported executable or a prediction for another campaign.

| Measurement | Reported result | Context |
|---|---|---|
| Startup | About 67 seconds reduced to 25 seconds. | Warm Steam startup with a large mod list, using the best of three runs. |
| File lookup workload | About 94% of roughly 1.4 million opens and metadata lookups failed. | The loading optimization avoids repeated failed filesystem requests using directory listings. |
| Empty combat records | About 4.5% less simulation time per day. | The measured save benefited from omitting records whose absence already means an empty result. |
| AI income targets | 10 billion loop rounds skipped with no reported verification mismatches. | An extreme modded save had previously suffered roughly half-hour freezes during monthly AI work. |

The original summary does not provide a complete hardware, mod-list, and timing-endpoint record
for these measurements. When publishing new results, include those details, the game and DLL
versions, relevant settings, repeated-run timings, and the benchmark log.

### Verification modes

Some performance options have a matching verification setting that compares optimized work
with the game's original implementation and logs discrepancies. The amount of checking varies
by feature: file lookup verification performs the real filesystem request, keyword-table
verification checks periodically, and income-target verification checks the increments of the
next two rounds rather than executing every skipped round. Consult
[Configuration](configuration.md) for the available switches.

Verification can add substantial overhead or remove the speed benefit entirely. Use it to
investigate correctness, and record whether it was enabled when reporting timings. A result
with zero mismatches describes the comparisons that ran, without proving equivalence for all
possible game states.

## Building and offline validation

Building requires Windows and Visual Studio 2022 or later, or the Visual Studio Build Tools,
with the C++ x64 tools installed. `build.bat` locates the toolchain through `vswhere`, builds the
DLL and native test programs, and copies the INI beside the output DLL. The normal build
command and output location are also covered in the [README](../README.md#building).

Use Python 3.10 or later to run the validation suite from the repository root:

```powershell
python tools/validate_builds.py --exe 'D:\Games\Stellaris\stellaris.exe'
```

Repeat `--exe` to validate several Windows installations in one run:

```powershell
python tools/validate_builds.py --exe 'D:\Steam\Stellaris\stellaris.exe' --exe 'D:\GOG\Stellaris\stellaris.exe'
```

For version-based discovery, place your local Windows game executables in `executables/`, using
names such as `stellaris_454_steam.exe` and `stellaris_454_gog.exe`, then run:

```powershell
python tools/validate_builds.py --version 4.5.4
```

`--version 454` selects the same files, and omitting both selectors discovers every matching
version in that directory. `--exe` and `--version` are mutually exclusive. Add `--skip-build`
only when the existing binaries correspond to the source you intend to validate.

The runner builds by default, executes standalone tests and Python arithmetic checks, and then
runs the executable-specific tests against each selected Windows image. Coverage includes
WinMM imports and exports, byte signatures, structure layouts, numeric calculations, and
installation of numeric patches in an offline harness. The script checks that native test
sources are registered in its matrix, so new tests must be added to the appropriate list.

Each run writes test logs and `report.json` under a new `build/validation-*/` directory. The
report includes executable hashes and the DLL hash when available. For the exact known Steam
4.5.2 executable, the runner automatically adds `--expect-known` to the location and multiplayer
pattern tests to check their expected addresses; other builds receive the normal pattern
checks.

Linux executables discovered under names such as `stellaris_454_linux` receive only an ELF
architecture check and a hash for inventory. They are not exercised by the Windows DLL tests,
and the runner still requires at least one Windows executable. Multiplayer harnesses use
test objects and transport substitutes, so their results do not establish live network behavior.

The authoritative test lists and report behavior are in
[tools/validate_builds.py](../tools/validate_builds.py). The
[release workflow](../.github/workflows/release.yml) builds the DLL and runs tests that do not
require game executables; the full executable-specific matrix must be run locally. Neither
workflow replaces live gameplay testing, and multiplayer has not been tested at all.

## Repository layout

| Path | Purpose |
|---|---|
| `src/dllmain.cpp` | DLL initialization, feature installation, game hooks, script integration, and benchmarking. |
| `src/locate.h` | Byte patterns and the code that derives addresses and offsets from them. |
| `src/allocator.h` | The memory allocator replacement installed before the game's C runtime starts. |
| `src/logging.h` | The structured log-record writer. |
| `src/uishortcuts.h` | Location and patch helpers for amount entry and queue shortcuts. |
| `src/pathcache.h`, `src/natsort.h`, `src/incometarget.h` | File lookup caching, natural name comparison, and income-target arithmetic. |
| `src/pdxstore.h`, `src/mpstore.h` | External script stores, serialization, and experimental multiplayer snapshot transport. |
| `src/mpchecksum.h` | The settings fingerprint included in the game's checksum. |
| `src/game.h` | Shared representations of game types. |
| `src/exports_winmm.h` | WinMM exports forwarded to the Windows system DLL. |
| `mods/` | Included Paradox-script mods, described in the [README](../README.md#included-mods). |
| `test/` | Native unit tests and executable-specific harnesses. |
| `tools/validate_builds.py` | The local validation runner. |
| `third_party/mimalloc/` | The bundled mimalloc library and its MIT license. |

Game executables are supplied locally for validation and are not distributed with this project.
