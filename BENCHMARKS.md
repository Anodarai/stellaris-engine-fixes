# Stellaris Engine Fixes benchmarks

## Late game on Steam 4.5.4

Measured on November 19, 2026, using a modded 4.5.2 save dated 2363.03.24.
The isolated benchmark profile used the save's 30 mods and disabled event auto-pause.

The [automated runner](tools/bench/README.md) measured all ten `[speed]` and five `[mimalloc]`
settings. All 60 launches passed and exited normally. Each setting had two pairs in `AB` and
`BA` order. Each arm started a fresh process, loaded the same save with `-loadrandomseed`,
warmed up for three game days at speed 5, and measured 90 days from 2363.03.27 to 2363.06.27.
The bundled INI supplied the baseline. Each pair varied one setting, with the same dependencies
and verification settings in both arms.

The A and B columns are geometric means of the two runs per arm. The change column is the
geometric mean of the paired B/A ratios, expressed as a percentage. Negative changes mean
less time. These results describe this machine, save, and mod profile. Two pairs give a limited
estimate of variability, and the individual settings can interact.

### All performance settings enabled versus disabled

A separate `--case all --save lateGame` run used the same fixture, profile, executable,
benchmark DLL and 90-day window. All four launches passed and exited normally. A disabled
all 15 benchmarked settings. B enabled them together, including the 33 ms frame interval,
10,000 ms purge delay, eager commit, large pages, and a 1 GiB huge-page reservation.
Both measured B runs obtained the full reservation. An earlier attempt fell back to ordinary
allocation for the reservation and was excluded from this run.

| Metric | All off | All on | Change | Pair 1 / Pair 2 |
|---|---:|---:|---:|---:|
| Wall time per game day | 325.46 ms | 261.70 ms | **−19.6%** | −19.5% / −19.6% |
| Launch to save-ready | 58.99 s | 44.16 s | **−25.1%** | −25.0% / −25.3% |
| Peak process commit | 15.48 GiB | 16.24 GiB | +4.9% | +5.2% / +4.6% |

### Wall time per game day

| Setting | A → B | A (ms/day) | B (ms/day) | Change | Pair 1 / Pair 2 |
|---|---:|---:|---:|---:|---:|
| `in_tick_frame_ms` | 0 → 33 | 293.32 | 265.61 | **−9.4%** | −9.3% / −9.6% |
| `batch_ticks` | 0 → 1 | 269.09 | 262.43 | −2.5% | −2.6% / −2.4% |
| `fast_intel_lookup` | 0 → 1 | 260.14 | 256.20 | −1.5% | −1.8% / −1.2% |
| `skip_empty_combat_entries` | 0 → 1 | 264.87 | 254.40 | **−4.0%** | −3.7% / −4.2% |
| `mega_afford_cache` | 0 → 1 | 257.81 | 252.58 | −2.0% | −1.7% / −2.4% |
| `fast_income_target` | 0 → 1 | 253.86 | 254.46 | +0.2% | +0.4% / +0.1% |
| `mimalloc.enabled` | 0 → 1 | 264.74 | 253.01 | **−4.4%** | −2.4% / −6.4% |
| `reserve_huge_os_pages` | 0 → 1 GiB | 252.03 | 251.08 | −0.4% | −0.1% / −0.6% |
| `allow_large_os_pages` | 0 → 1 | 252.41 | 257.13 | +1.9% | +2.9% / +0.9% |
| `arena_eager_commit` | 0 → 1 | 251.33 | 254.40 | +1.2% | +2.0% / +0.4% |
| `purge_delay` | 0 → 10,000 ms | 254.85 | 256.04 | +0.5% | +2.3% / −1.3% |

Enabling mimalloc increased peak process commit from 13.86 to 14.48 GiB (+4.5%). Enabling
eager commit with ordinary pages increased it from 14.32 to 14.91 GiB (+4.1%). The large-page
toggle ran with eager commit active in both arms. Both reservation-enabled runs obtained the
requested 1 GiB. The purge-delay pairs disagreed on direction.

### Launch to save-ready

| Setting | A → B | A (s) | B (s) | Change | Pair 1 / Pair 2 |
|---|---:|---:|---:|---:|---:|
| `fast_file_lookups` | 0 → 1 | 55.19 | 46.58 | **−15.6%** | −14.7% / −16.5% |
| `fast_token_rebuild` | 0 → 1 | 46.26 | 45.83 | −0.9% | −1.5% / −0.3% |
| `fast_deferred_unregister` | 0 → 1 | 47.74 | 45.87 | −3.9% | −4.1% / −3.8% |
| `skip_modifier_docs` | 0 → 1 | 48.69 | 46.16 | −5.2% | −4.6% / −5.7% |

### Reproduction

To run the same comparisons on your own campaign, supply a late-game save and a matching
profile, then run the [benchmark runner](tools/bench/README.md) with `--save lateGame --pairs 2`.
The runner writes `results.json` with per-arm measurements and `manifest.json` with the input
hashes under `build/benchmarks/`.

| Input | SHA-256 |
|---|---|
| Steam 4.5.4 executable | `10d2613a392fe8ac7982701899dc2e43ebf4c1c721ea1baeba6c3fb25db36eb1` |
| SEF release benchmark DLL | `bcb6c5ca24a526a83ebdd238d62c0b640d2b1597915590c9a3d234c88f19de46` |
| Baseline INI | `f8e8e5d69fe8bd970b4d4cece2d7c3b0bbc112e70bf330e51e3995e1ee2c4a9e` |
| `lateGame.sav` | `38481eb1918472093656c6c9edc6c15feaf537c462d77b57d99ea517db4b9540` |
