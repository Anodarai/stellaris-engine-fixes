# Configuration reference

Edit `stellaris-engine-fixes.ini` beside `winmm.dll` in the game folder, then restart Stellaris for the changes to take effect. The defaults below are those in the [supplied configuration](../stellaris-engine-fixes.ini); use `0` to disable a switch and `1` to enable it unless its description specifies another value.

**Multiplayer has not been tested at all, and everything else has had limited testing.** The descriptions below explain the implemented behavior, including experimental options, without guaranteeing compatibility with every game version, save or combination of mods.

The default configuration enables interface improvements, performance changes, the replacement memory allocator and script storage. All AI and numeric fixes are disabled, as are verification modes, large-page allocation and multiplayer script storage. Keep the section headers when editing, because settings such as `enabled` have different meanings in different sections.

## Interface — `[interface]`

| Setting | Default | Effect |
|---|---:|---|
| `trade_amount_entry` | `1` | Alt-click an amount or its **+** or **−** button in an empire deal or the market's monthly trade window to enter an amount in the game's text popup. The input receives keyboard focus automatically. |
| `planet_queue_first` | `1` | Ctrl-click when adding planet construction to place the new item at the front of that planet's queue. This shortcut is disabled in multiplayer. |

Fleet and megastructure outliner sorting currently have no individual configuration switches. They remain enabled while the DLL is installed and their required hooks are available.

## Memory allocation — `[mimalloc]`

The replacement allocator changes how the game obtains and releases memory. Large pages are optional and remain disabled in the supplied configuration.

| Setting | Default | Effect |
|---|---:|---|
| `enabled` | `1` | Replaces the game's memory allocator with mimalloc at startup. Set this to `0` to retain the game's allocator while keeping the other features enabled. |
| `reserve_huge_os_pages` | `0` | Amount of memory, in GiB, to reserve at startup using huge pages, with a fallback to smaller large pages. `0` disables the reservation. Reserved memory remains committed to the game until it exits. |
| `allow_large_os_pages` | `0` | Allows mimalloc to use 2 MiB large pages for other allocations. This also requires `arena_eager_commit` to be `1` or `2`, together with the Windows privilege described below. |
| `arena_eager_commit` | `0` | Controls when mimalloc commits arena memory. The supplied `0` commits memory as needed; `1` commits whole arenas upfront, which can substantially increase committed memory and pressure on RAM and the page file. Huge-page reservations are committed upfront regardless of this setting. |
| `purge_delay` | `10000` | Delay in milliseconds before mimalloc attempts to release unused memory to Windows; the supplied value is 10 seconds. |

Large pages require the Windows **Lock pages in memory** user right. On systems with Local Security Policy, this is under `secpol.msc` → **Local Policies** → **User Rights Assignment**; restart Windows after changing it. Keep reservations below the available physical memory, allowing room for Windows and other programs. Fragmentation can prevent a large reservation even when enough total memory is free, and the startup log records how much was actually reserved. The allocator uses ordinary pages for memory it cannot obtain this way.

Additional mimalloc options can be added to this section using their option names without the `MIMALLOC_` prefix. For those forwarded options, an existing matching environment variable takes precedence over the INI value. The mod handles `enabled` and `reserve_huge_os_pages` itself.

## Performance — `[speed]`

These options are intended to reduce repeated work while preserving simulation results. Frame pacing also trades some visual smoothness during simulation for faster game days, so its effect is different from a general increase in rendering performance.

### Simulation and rendering

| Setting | Default | Effect |
|---|---:|---|
| `in_tick_frame_ms` | `33` | Minimum interval, in milliseconds, between frames drawn from inside the simulation. The default allows roughly 30 such frames per second during long ticks; larger values reduce this rendering work but can make those periods look choppier. `0` restores the game's timing, and values are limited to `0`–`1000`. This is not a general cap on the game's frame rate. |
| `batch_ticks` | `1` | At the fastest game speed, groups the day's ten simulation ticks into two batches instead of the usual five, preserving the boundaries where AI and daily commands are processed. This is disabled in multiplayer. |
| `fast_intel_lookup` | `1` | Caches cloaking-detection information during each automated-fleet pathfinding search instead of repeatedly scanning the same lists. |
| `skip_empty_combat_entries` | `1` | Skips creating empty combat records for fleets that have no combat participants, because a missing record already represents the same empty state. |
| `mega_afford_cache` | `1` | Checks whether an AI empire can afford a megastructure once per placement search instead of repeating the check for each candidate system. |
| `fast_income_target` | `1` | Calculates how many identical rounds the AI's income-target loop needs, avoiding very long pauses in extreme economies. It falls back to the game's loop when the shortcut cannot represent the result safely. The separate `income_target_arithmetic` fix takes precedence when installed. |

The single-player restriction on `batch_ticks` does not also disable `in_tick_frame_ms`, although multiplayer behavior remains untested for the entire mod.

### Loading

| Setting | Default | Effect |
|---|---:|---|
| `fast_file_lookups` | `1` | Uses cached folder listings to answer repeated unsuccessful file lookups while the game searches its own files and mounted mods. |
| `fast_token_rebuild` | `1` | Extends the script keyword text table with newly discovered entries instead of rebuilding the entire table whenever a keyword is added. |
| `fast_deferred_unregister` | `1` | Avoids scanning the deferred-reference list when an object is known to have no registered entries. |
| `skip_modifier_docs` | `1` | Skips generating the modifier reference in `logs/script_documentation/modifiers.log`. Set it to `0` if you need that documentation for mod development. |

### Verification modes

These switches add checks to their corresponding enabled performance feature and write mismatches to `stellaris-engine-fixes.log`. They add work and can remove the performance gain, so they are intended for diagnosis rather than ordinary play. A verification switch does not enable its corresponding feature by itself.

| Setting | Default | Check performed when enabled |
|---|---:|---|
| `fast_intel_verify` | `0` | Compares each cached detection lookup with the game's original lookup and uses the original answer. |
| `mega_afford_verify` | `0` | Compares each cached affordability answer with the game's original check and uses the original answer. |
| `income_target_verify` | `0` | Lets the second and third income-target rounds run and checks that they add the same amounts as the first before skipping later rounds. It does not replay the full skipped loop. |
| `fast_file_lookups_verify` | `0` | Checks cached missing-file answers against the file system and uses the file system's answer. |
| `fast_token_rebuild_verify` | `0` | Runs the game's full keyword-table rebuild on every sixteenth incremental call and compares the resulting strings. |
| `fast_deferred_unregister_verify` | `0` | Checks proposed skips against the actual deferred-reference list and calls the original unregister operation if a supposedly absent entry is found. |

The console command `sef_bench [seconds]` measures game-day throughput and time spent in simulation and rendering. It is unavailable in Ironman and multiplayer; command details and log formats are in [Development and diagnostics](development.md).

## Numeric fixes — `[numeric_fixes]`

All settings in this section default to `0`. These fixes address selected overflow paths that can become relevant with very large values, particularly in heavily modded games. With the exception of `military_power_display`, they can change simulation results and enter the mod's settings checksum when enabled.

The fixes preserve the save format, but changed resource totals, calculations and resulting decisions can be saved normally. They do not enlarge every engine field or repair values already corrupted in a save, and they do not cover every arithmetic operation in Stellaris. Where a result cannot fit its existing storage, the applicable fix limits it to the representable range; paired fleet health totals may instead be scaled together to preserve their ratio.

| Setting | Default | Effect and scope |
|---|---:|---|
| `income_target_arithmetic` | `0` | Uses exact recursive income-target sums and scaling-round counts, including optional scaling plans, then limits values when returning a resource table. It supersedes `fast_income_target` and stops unreachable mixed-sign targets after one scaling round, which changes those formerly nonterminating cases. |
| `income_production_arithmetic` | `0` | Keeps parallel income and category sums in a wider representation until their production passes are combined. This covers the production additions and the associated reads, resets and drains. |
| `income_forecast_arithmetic` | `0` | Protects forecast calculations for income plus projected production minus upkeep, along with differences between forecast targets. It does not replace `income_production_arithmetic`. |
| `resource_amount_rounding` | `0` | Prevents whole-unit amounts passed into resource addition and related situations from wrapping at the signed 32-bit boundary, such as amounts above 2,147,483,647. It preserves normal resource caps and fractional truncation, but does not by itself protect later additions, conversions or income calculations. |
| `fixed_point_conversion` | `0` | Prevents narrowing to 32 bits in the binary-to-decimal fixed-point conversion helper and the fleet/technology conversions used for base diplomatic weight. It preserves fractional rounding and limits results at the signed 64-bit boundary, without covering later weight multipliers or every display conversion. |
| `fleet_power_arithmetic` | `0` | Protects fleet-power sums, health-weighted power calculations and country totals, together with ship-power calculations, damage/component inputs and relevant ship-design caches. Results are limited when written back to the game's existing fields. |
| `resource_multiplication` | `0` | Protects signed fixed-point multiplication through `CResourceTable::MultResource`, limiting the result after division. It does not cover separate inlined economic calculations or stockpile additions. |
| `stockpile_arithmetic` | `0` | Protects single and batch stockpile additions/removals and resource conversion before the game's normal capacity handling. It also covers whole-unit rounding and removal at the signed 64-bit minimum. Changes for resources outside the stockpile table are skipped and counted. After a batch arithmetic fault, slot checks remain active; a batch with an invalid slot, or whose check faults, is skipped as a whole. |
| `military_power_display` | `0` | Keeps large military-power values in the game's 64-bit abbreviated-number formatting path while retaining ordinary formatting, boss labels and colors. This changes the display only, does not correct the underlying power calculation, and is excluded from the settings checksum. |
| `diplomatic_weight_arithmetic` | `0` | Protects base and final diplomatic weight, component/modifier totals, economic and technology scores, job weights and favor loans. It also covers vote sums, federation decisions and affected tooltip values, while retaining the game's final nonnegative limit. This option also installs the shared `fixed_point_conversion` fix even if that separate switch is `0`. |
| `fleet_stat_arithmetic` | `0` | Protects maximum hull, armor and shield calculations and their fleet totals. When current and maximum totals cannot both fit, it scales the pair together to preserve the health ratio; ordinary totals retain their original values. |

## Script storage — `[script_storage]`

Script storage lets mods retain numbers and recorded script in files under `sefdata` beside the game executable. Enabling the service alone does not change gameplay; a mod must use its commands. See [Script storage and included mods](script-storage.md) for usage and persistence details.

| Setting | Default | Effect |
|---|---:|---|
| `enabled` | `1` | Enables the `log = "@sef:..."` storage commands, including a global store shared by games and a store associated with each save. Set it to `0` to disable the service. |
| `multiplayer` | `0` | Enables experimental distribution of the host's stores to clients when a multiplayer game starts or a player joins. It requires storage to be enabled, and every player needs the same DLL and setting. Only the host persists data, while clients use temporary copies; recorded-script `run` commands remain disabled. |

Multiplayer storage has not been tested in a multiplayer game. Its implementation limits combined snapshots to 1 MB and sends a storage-disable message when that limit is exceeded. Host migration, localized recordings and concurrent script writes are not supported. Setting `multiplayer=1` also enters the settings checksum when storage is enabled.

## AI behavior — `[ai_fixes]`

Every AI option defaults to `0` because these changes affect how AI empires play. They preserve the save format, while their effects on the ongoing game can appear in subsequent saves. Enabled options are included in the settings checksum by default; multiplayer remains entirely untested.

| Setting | Default | Effect |
|---|---:|---|
| `build_plan_keep` | `0` | Keeps a planet build plan until the AI replaces it the following month, rather than discarding it after the first weekly pass. Items whose planets are unavailable or already building something move to the end so other items can be attempted. |
| `build_plan_skip_unaffordable` | `0` | Lets the weekly build-plan pass continue after an item it cannot order, including an unaffordable item. This can make the AI buy cheaper items instead of saving for the highest-priority one. |
| `opportunist_wars` | `0` | Makes opportunist AI empires account for a prospective target's existing enemies when considering war, adding half those enemies' fleet power to the AI's side of the comparison. |
| `war_distance_slope` | `0` | Applies the war-declaration distance penalty per jump beyond the configured threshold, instead of calculating it from the whole distance and immediately reaching the minimum score with the normal defines. |
| `war_plan_check` | `0` | Checks the game's declaration rules before preparing for war and while a war plan is active. Cancelling an invalid plan requests a military-strategy refresh so local hostiles can become targets again. |
| `boss_threat_floor` | `0` | Uses a hostile boss's cached fleet power when it exceeds the game's `BOSS_MILITARY_POWER` estimate, so a stronger boss is not treated as having only that fixed amount of power. |
| `truce_war_divisor` | `0` | Sets the divisor applied to the AI's chance of preparing for war when it has a truce with any country. `0` keeps the game's define; a positive integer overrides it. `4` matches the unmodified define, lower positive values weaken the penalty, and `1` removes this division penalty without removing truce rules. |
| `expansion_refresh_monthly` | `0` | Keeps the AI's colonisation and expansion-related target lists on a monthly refresh throughout the game, instead of switching to yearly refreshes after the initial period. The affected data also includes exploration, stations, outposts and terraforming. |
| `ai_schedule_from_date` | `0` | Ties weekly, monthly and yearly AI updates to the game date and country ID, preventing their schedule from shifting whenever a save is loaded. |
| `ai_random_from_save` | `0` | Restores the AI's saved random stream without requiring the `-loadrandomseed` launch option, instead of restarting it at the country ID. This is specific to the AI's stream and does not independently restore every random generator in the game. |
| `colonizer_idle_days` | `0` | After an AI colony ship has been idle for this many game days, it stops blocking construction of another colony ship. `0` keeps the game's behavior; the supplied comments suggest `180` as a starting value. The normal ship/queued-ship cap relative to available targets remains, but additional idle ships can accumulate. |
| `constructor_idle_days` | `0` | After this many game days without finding work, an AI construction ship searches with half the normal distance penalty; after twice that time, it searches with no distance penalty. It still considers only targets on the AI's existing lists. `0` keeps the game's behavior; the supplied comments suggest `60` as a starting value. |
| `survey_blacklist_monthly` | `0` | Clears the list of systems that AI science ships could not survey each month instead of each year, allowing previously blocked systems to be reconsidered sooner. It applies to all listed systems, whether or not the AI has current intel. |

## Diagnostics — `[diagnostics]`

These two settings work together to trace one AI empire's fleet decisions for one system. Use decimal IDs from the save; leaving either setting at `-1` disables the trace.

| Setting | Default | Effect |
|---|---:|---|
| `ai_fleet_country` | `-1` | Selects the country whose military planning is traced. |
| `ai_fleet_system` | `-1` | Selects the system to trace, including target discovery, required fleet power and fleet assignment. |

The output is written to `stellaris-engine-fixes.log` beside the DLL. See [Development and diagnostics](development.md) for log levels, formatting and benchmark commands.

## Multiplayer checksum — `[multiplayer]`

**Multiplayer has not been tested at all.** The checksum mechanism is implemented to distinguish configurations that can produce different simulation results, and is not a claim that matching configurations have been verified to remain synchronized.

| Setting | Default | Effect |
|---|---:|---|
| `settings_in_checksum` | `1` | Adds enabled gameplay-changing settings to the game's checksum at startup. `0` leaves the checksum unchanged by this mechanism even when those settings differ, removing the intended compatibility check. |

The fingerprint includes nonzero settings in `[numeric_fixes]` and `[ai_fixes]`, except the display-only `military_power_display`. It also includes `[script_storage] multiplayer=1` when storage is enabled. Unknown nonzero keys in the two fixes sections are included as well, so avoid leaving unused experimental settings there. With none of the included settings enabled, this mechanism does not change the game's checksum.

The checksum records configured settings rather than proving that every requested hook installed successfully. Check the startup log for failures, particularly after a game update. Different settings are intended to prevent players from joining the same game, but this behavior and live synchronization still need multiplayer testing.

A changed checksum is expected to affect achievement eligibility, but achievement behavior has not been verified in a live game. An unchanged checksum is likewise not a guarantee of achievement compatibility.

Return to the [README](../README.md) for installation, the feature overview and included mods.
