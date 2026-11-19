# Live performance A/B benchmarks

The runner benchmarks the current WinMM build of Stellaris Engine Fixes on Windows. It starts
a fresh game for every arm, loads a fixed save, runs at speed 5, measures an exact date range,
and closes the process before the next launch. It repeats A/B in alternating order for each
setting and save. See [the fixture list](saves/README.md) for the four save names and measurement windows.
Supply your own saves.

## Run

Run `.\build.bat release` first. Close Stellaris and leave Steam running
for the Steam build. Preview the cases and fixture availability.

```powershell
python tools\bench\run.py --plan
```

Once the named saves are present, run a short pair or the full matrix.

```powershell
python tools\bench\run.py --game-dir 'D:\Games\Stellaris' --case speed.batch_ticks --save earlyGame --pairs 1 --days 10
python tools\bench\run.py --game-dir 'D:\Games\Stellaris'
```

`STELLARIS_DIR` can supply the game directory. Repeat `--case` or `--save` to select a subset.
The default matrix is 16 cases × 4 saves × 2 pairs, or 256 launches. Run the bundle comparison
alone with `--case all --save lateGame`.
`--profile-template` can supply graphics, DLC, notification and alert settings, plus
`mod\*.mod` descriptors for a modded fixture. Keep those files, installed mods, and DLC stable
throughout a suite. The stock benchmark profile uses the game's default graphics settings
and no mods. The executable, DLL, baseline INI and fixture hashes go into `manifest.json`.

## Comparisons

The baseline is the repo's INI or the file passed to `--ini`. The matrix covers the ten
optimizations in `[speed]` and the five documented allocator settings in `[mimalloc]`.
Verification switches and SEF script storage are off in both arms. The four load-time cases
report launch-to-save-ready time as their primary metric. Both arms also record the
fixed-day simulation, frame breakdown and peak process memory.

Each individual comparison varies one setting. The frame interval is 0 versus 33 ms, the purge delay is
0 versus 10,000 ms, and huge-page reservation is 0 versus 1 GiB by default
(`--huge-pages-gib` sets the second value). The allocator sub-options run with mimalloc
enabled. The large-page toggle uses eager commit in both arms, and the eager-commit
comparison uses ordinary pages. The reservation reports an unavailable arm when Windows
cannot reserve the requested amount. The large-page toggle also reports any failure to
enable large pages. For `fast_income_target`, both arms
disable the numeric fix that supersedes it.

The `all` case runs the full performance bundle. A disables all 15 settings, including
mimalloc. B enables all 15 using the comparison values above, including eager commit,
large pages and the requested huge-page reservation. It reports both wall time per game
day and launch-to-save-ready time. If Windows falls back from huge or large pages, the bundle
reports the fallback alongside the timings. Verification switches stay off in both arms.

## Files and results

Every launch uses a temporary game profile with a fresh copy of the fixture under
`save games\SEF Benchmark`. The runner writes its own `continue_game.json` there and redirects
the game with `userdir.txt`. It temporarily supplies the benchmark DLL and INI in the game
directory and restores the previous files on exit, including after a timeout or Ctrl+C.
The runner sends a close request first. If the game stays open, it terminates only the process
it launched and records the shutdown method. It uses the saved-seed option on both arms.

The default output directory is `build\benchmarks`. Each suite keeps
the test INIs, raw logs and `results.json` with individual runs and paired percentage changes.
Only complete pairs contribute to the comparison. A negative change means the measured
time or memory was lower with B. Run additional pairs when differences are small.
The runner reports a pause, a changed date range, or a failed hook installation as an error
for that arm and continues with the remaining launches.

To stop a running suite, create an empty `CANCEL` file in its output directory. The runner
closes the current benchmark process and restores the installation.

If the runner is interrupted before it can restore the game files, close Stellaris and check
`.sef-bench.lock` in the game folder. It points to `original-game-files` in the suite output.
`files.json` shows which files existed before the suite. Restore those files from the backup,
remove any replacements for entries marked false, then remove the lock.
