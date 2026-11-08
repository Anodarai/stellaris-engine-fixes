# stellaris-engine-fixes

Performance, load-time, interface and AI fixes for Stellaris, as a DLL the game loads itself
(Steam, Windows; written against 4.5.2, checked on 4.5.3).

`winmm.dll` goes in the game folder; Windows loads it into the game. Each feature finds the code
it needs by byte patterns, so it should survive updates that don't change that code. A feature
whose patterns are not found turns itself off, and the others keep working. Nothing in the game's
files is changed.

Settings are in `stellaris-engine-fixes.ini`. Speed features that have a `..._verify=1` setting can also run
the game's own code next to the shortcut and log any difference to `stellaris-engine-fixes.log`.

## Interface

- **Fleet outliner order:** fleets sorted by name, naturally ("Fleet 2" before "Fleet 10"); fleets
  with the same name by their oldest ship's construction date.
- **Megastructure outliner order:** megastructures sorted by their current name, the same way;
  same-name ones stay in the order they were acquired.

## Performance

None of these change what the game computes; they skip repeated or wasted work.

**Loading** (warm Steam startup with a large mod list: 67 s to 25 s, best of three)
- **File lookups** (`fast_file_lookups`): PhysFS looks for every file in each mounted mod folder
  in turn, and about 94% of the ~1.4 million opens and stats fail. They are answered from folder
  listings read once, through hooks on the exe's import table.
- **Script keywords** (`fast_token_rebuild`): `CStaticLexer` refills its whole keyword text
  table whenever a new keyword appears while mods load; only the new entries are added.
- **Deferred references** (`fast_deferred_unregister`): while loading, objects that never
  registered a deferred reference skip the scan of the whole list when they unregister.
- **Modifier documentation** (`skip_modifier_docs`): `logs/script_documentation/modifiers.log`,
  a list for modders, isn't written.

**Running the game**
- **Memory allocator** (`[mimalloc]`): the game's statically linked CRT heap is replaced with
  mimalloc 3.6.1 before the game starts; large pages are optional.
- **Frames per game day** (`batch_ticks`, `in_tick_frame_ms`): at the fastest speed the game runs
  only 2 ticks per frame, and inside long ticks it renders whole frames at the monitor's refresh
  rate. The DLL runs a day's ticks in 2 frames instead of 5 (batches still end at the AI's tick
  and the day's tick) and limits frames inside ticks to one per 33 ms. Single player only.
- **Combat bookkeeping** (`skip_empty_combat_entries`): every tick the game inserts an empty combat
  record for every fleet; empty ones aren't stored, since a missing record already reads as
  empty. Measured: 4.5% less simulation time per day.
- **Fleet automation** (`fast_intel_lookup`): automated fleets' pathfinding asks, for every
  system it passes, how well each empire detects cloaked fleets there; the answers come from a
  table built once per search instead of a scan of two lists.
- **AI megastructure placement** (`mega_afford_cache`): an AI choosing where to build a
  megastructure checks whether it can afford it once per search instead of once per candidate
  system.
- **AI income targets** (`fast_income_target`): each AI month an empire's economic plan adds the
  income of its "scaling" subplans to a total, round after round, until some resource passes a
  target taken from the empire's forecast, with no limit on the rounds. A very large economy in an 
  extremely modded game can need over a billion rounds: a seen case froze the game for half an hour
  every AI month. Everyround adds the same amount, so the DLL works out the round count after the 
  first round and sets the total and the counter to what the game's rounds would reach. Verified on
  that save: 10 billion rounds skipped, 0 differences.

**Measuring:** the console command `sef_bench [seconds]` (default 60) reports days and ticks per
second and how the main thread's time splits between the simulation, frames inside ticks, and
frames between ticks. `sef_bench stop` ends a run, `sef_bench show` prints the last one again, and
`sef_bench combat off|on` switches the combat change while the game runs, for A/B comparisons.

## AI fixes (`[ai_fixes]`, each off by default)

These are places where the country AI's code disagrees with itself or with its own defines. Unlike the speed features they change how AI empires play, so each one is opt-in.
They act on the AI only and don't change saves. Single player; multiplayer is untested.

- **Build plan** (`build_plan_keep`): the AI makes a planet build plan once a month, uses it for a
  week, then deletes it, and its walk of the plan stops at the first item it can't place. With
  the fix the plan lasts the month, and items whose planet is busy move to the end instead of
  blocking the rest.
- **Opportunist trait** (`opportunist_wars`): described as "more likely to attack someone already
  at war", but the code path never counts that. With the fix, half the fleet power of a target's
  enemies in its current wars is added to the AI's side.
- **War distance** (`war_distance_slope`): past `AI_WAR_DECLARATION_MALUS_DISTANCE` the game
  applies one flat penalty, the same for a target one jump past it as for one across the galaxy.
  With the fix the penalty grows with each jump past the distance.
- **Truce divisor** (`truce_war_divisor=N`): a truce divides the AI's war appetite by
  `AI_AGGRESSIVENESS_BOXED_IN_MULT` (4). The fix makes the divisor its own setting; 4 plays as
  vanilla, lower values weaken the effect.
- **Expansion lists** (`expansion_refresh_monthly`): the AI rebuilds its list of colonisation
  targets monthly for the first 15 years and yearly afterwards. The fix keeps it monthly.
- **AI schedule** (`ai_schedule_from_date`): each AI's weekly, monthly and yearly updates fall on
  days counted from when the game started or was loaded, so every load reshuffles them. The fix
  ties them to the date: days where (country id + day) is a multiple of 7, 30 or 360.
- **AI random** (`ai_random_from_save`): a loaded AI restarts its random number stream at its id
  unless the game runs with `-loadrandomseed`. The fix makes it continue the stream stored in the
  save.

## Script storage for mods (`[script_storage]`)

Paradox script can keep data outside the game, in the `sefdata` folder in the game folder, with
`log = "@sef:..."` effects and triggers. Nothing happens unless a mod uses them; without the DLL
they are ordinary `log` lines. Off in multiplayer, where each machine would read different files.

Two stores: `global`, shared by every game (`sefdata\global.txt`), and `save`, which follows the
save game (`sefdata\saves\<folder>\<name>.txt`: written when the game saves, read when that save
loads, empty in a new game). Keys and variable names use letters, digits and `_ . : @ -`.

| After `@sef:` | Where | What |
|---|---|---|
| `global.set KEY = VARIABLE` | effect | stores this scope's variable (an unset one unsets the key) |
| `global.set KEY = 12.5`, `global.set VARIABLE` | effect | stores a number; `VARIABLE = VARIABLE` |
| `global.get VARIABLE = KEY`, `global.get VARIABLE` | effect | sets this scope's variable from the store; nothing if the key is unset |
| `global.clear KEY` | effect | unsets the key |
| `global.has KEY` | trigger | whether the key is set |
| `global.record KEY` ... `record_end` | effect | keeps every `log` line in between, as the game writes it (`\[...]` filled in); lines without `=`, `{` or `}` are dropped |
| `global.run KEY` | effect | runs the recorded lines as an effect at the end of the frame, through the console's `effect` command (on the player's country, or what the interface has selected) |

`save.` instead of `global.` uses the save store. Problems (a bad command, a full store) are
written to `stellaris-engine-fixes.log`.

```
immediate = {
    log = "@sef:global.get times_played"
    change_variable = { which = times_played value = 1 }
    log = "@sef:global.set times_played"
}
```

`mods/sef_giga_user_preset` uses it for Gigastructural Engineering: an edict, **Save Settings as
My Preset**, records the settings with Gigastructures' own preset printer, and the menu's **User
Preset** button loads them in a later game. Copy the folder into `Documents\Paradox
Interactive\Stellaris\mod`, add it in the launcher, and load it after Gigastructures.

Status: the patterns are checked on Steam 4.5.2 and 4.5.3 and on GOG 4.5.2; not yet run in the game.

## Build

Needs Visual Studio (2022 or later, any edition, or the Build Tools) with the C++ x64 tools;
`build.bat` finds it with `vswhere`.

```bat
build.bat
```

## Install

Close the game first. Take `winmm.dll` and `stellaris-engine-fixes.ini` from a release zip, or
build them (`build\winmm\`), and copy both into the folder that holds `stellaris.exe`. The DLL
writes `stellaris-engine-fixes.log` next to itself.

## Uninstall

Delete `winmm.dll` from the game folder. To keep the rest but go back to the game's own heap, set
`enabled=0` under `[mimalloc]` in `stellaris-engine-fixes.ini` instead.

## Tests

```bat
build\natsort_test.exe
build\pathcache_test.exe
build\income_test.exe
build\proxy_test.exe
build\pdxstore_test.exe
build\locate_test.exe <path to stellaris.exe> --expect-known
build\alloctest\alloc_host.exe
```

`--expect-known` also checks every located address against the 4.5.2 Steam build; without it the
test only requires every pattern to be found.

## Layout

| Path | What |
|---|---|
| `src/dllmain.cpp` | The features and their hooks. |
| `src/locate.h` | The byte patterns, and the code that reads addresses and offsets out of them. |
| `src/allocator.h` | The heap replacement, installed from `DllMain` before the game's CRT starts. |
| `src/pathcache.h`, `natsort.h`, `incometarget.h`, `pdxstore.h`, `game.h` | File lookup cache, name comparison, the income target arithmetic, the script storage's stores and files, game types. |
| `mods/sef_giga_user_preset` | A Gigastructures submod that saves and loads a settings preset with the script storage. |
| `src/exports_winmm.h` | The `winmm.dll` exports, forwarded to the system copy. |
| `test/` | The tests above. |
| `third_party/mimalloc` | mimalloc 3.6.1 (MIT, see its `LICENSE`). |
