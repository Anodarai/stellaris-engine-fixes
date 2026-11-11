# Stellaris Engine Fixes

## Overview

Stellaris Engine Fixes is a Windows DLL mod for the Steam and GOG versions of Stellaris. It
improves loading and simulation performance, adds interface shortcuts and outliner sorting,
and offers optional AI and numeric overflow fixes. The DLL modifies the running game in memory
without changing the original executable on disk.

**Multiplayer has not been tested at all, and all other functionality has had only limited
testing.** Offline compatibility checks cover Steam and GOG builds 4.5.2 through 4.5.4, but
those checks do not establish that every feature works correctly during gameplay.

Download the [latest release](https://github.com/Anodarai/stellaris-engine-fixes/releases/latest)
to get the DLL, its configuration file and the optional launcher mods described below.

## Features overview

### Interface

- **Fleet sorting:** sorts the outliner by name, with numbers in natural order, so “Fleet 2”
  appears before “Fleet 10”. Fleets with the same name are ordered by their oldest ship's
  construction date, which can change when ships are lost or transferred.
- **Megastructure sorting:** sorts by the current displayed name, keeping acquisition order
  when names are equal.
- **Direct trade amounts:** Alt-click an amount or its **+** or **−** button in an empire trade
  deal or the market's monthly trade window to type a number directly into a focused input.
- **Construction priority:** Ctrl-click to add a new construction item at the front of a
  planet's queue; this shortcut is available in single player only.

### Performance

These features are enabled by default and are intended to preserve the simulation's results.

- **Faster loading with mods:** reduces repeated file searches and script-loading work, and
  skips generating the modifier documentation log. One measured modded setup went from about
  67 seconds to 25 seconds on warm starts; results depend on the mod list and system.
- **Faster simulation at maximum speed:** spends less time drawing frames between and during
  simulation ticks. Tick batching is available in single player only, and reducing frames
  during long ticks can make the display less smooth.
- **Less repeated simulation work:** reduces unnecessary combat bookkeeping and repeated
  calculations in fleet automation and AI megastructure placement.
- **Shorter AI income calculations:** avoids extremely long monthly freezes caused by an
  income-target calculation repeating the same work in very large modded economies.
- **Replacement memory allocator:** uses mimalloc for the game's memory allocations, with
  additional memory options available in the configuration reference.

### Optional AI fixes

AI fixes are **disabled by default** because they change how AI empires play. They cover:

- Keeping construction plans for the full month and processing other items when one is
  blocked or unaffordable.
- War planning, distance penalties, opportunist behavior and assessment of hostile boss fleets.
- More frequent expansion and survey updates, and handling colony or construction ships that
  have been idle for a long time.
- Consistent scheduling of AI updates and restoring the AI's saved random-number stream.

### Optional numeric fixes

Numeric fixes are **disabled by default** and address overflow and rounding in resource
stockpiles, income, diplomatic weight, military power and fleet or ship statistics. They are
intended for extreme values, such as those reached in heavily modded games, and can change
simulation results where the original calculations overflow. A separate option fixes the
display of very large military-power values.

### Support for other mods

Mods can use persistent storage shared across campaigns or associated with individual saves.
The included Gigastructures preset mod uses it to reuse settings in later games. Storage is
enabled by default but does nothing unless a mod uses it; the [script storage reference](docs/script-storage.md)
describes the API and its limitations.

See the [configuration reference](docs/configuration.md) for every setting, its default and
its effect, and the [developer guide](docs/development.md) for implementation and validation details.

## Using

### Install or update

1. Close Stellaris and download the ZIP attached to the
   [release you want to use](https://github.com/Anodarai/stellaris-engine-fixes/releases).
   If updating, back up your existing `stellaris-engine-fixes.ini` before replacing any files.
2. Extract `winmm.dll` and `stellaris-engine-fixes.ini` into the game installation folder,
   beside `stellaris.exe`. In Steam, use **Manage → Browse local files** to find that folder.
3. Launch the game normally. The DLL loads automatically and writes
   `stellaris-engine-fixes.log` beside itself; the log reports which features were installed
   and any problems encountered.

When updating, apply your chosen settings from the backup to the new INI so that new options
are available. The optional mods in the ZIP's `mods` folder are installed
separately through the launcher, as described under [Included mods](#included-mods).

### Configure

Edit `stellaris-engine-fixes.ini` while the game is closed, then restart the game to apply your
changes. The supplied defaults enable the interface and performance features while leaving AI
and numeric fixes off. Read the [configuration reference](docs/configuration.md) before enabling
those optional changes or adjusting memory and frame-pacing settings.

The mod does not introduce a new game-save format, although changes to gameplay are saved as
part of the campaign normally. Mods using script storage also keep data in the separate
`sefdata` folder beside the DLL; keep that folder with your saves if you need to preserve or
move their stored data.

Gameplay-changing options are included in the game's checksum by default. Multiplayer has not
been tested, and achievement compatibility has not been verified; the configuration reference
explains the relevant settings and restrictions.

### Troubleshoot or uninstall

If a feature is unavailable, check `stellaris-engine-fixes.log` for warnings and errors. The
log is rewritten at each launch, so copy it before restarting when reporting a problem, along
with your game version, store and configuration.

To uninstall, close the game and delete `winmm.dll` from the game folder. You can keep the INI,
log and `sefdata` folder if you want to retain settings or stored mod data. Remove any optional
launcher mods separately if you no longer want to use them.

To disable only the replacement memory allocator while keeping the other features, set
`enabled=0` in the `[mimalloc]` section of the INI.

## Building

Install Visual Studio 2022 or later, or the Visual Studio Build Tools, with the C++ x64 tools
and a Windows SDK. From the repository root, run:

```bat
.\build.bat
```

The script finds the installed build tools automatically and creates `winmm.dll` and
`stellaris-engine-fixes.ini` in `build\winmm\`. Copy those two files into the game folder as
described above. It also builds the test programs.

For offline validation against your game executable, see the [developer guide](docs/development.md).
The validation runner additionally requires Python 3.10 or later.

## Included mods

The release ZIP also contains optional, normal Stellaris mods in its `mods` folder. Install and
enable them in a launcher playset separately from the DLL.

### AI claims dead-end systems

[AI claims dead-end systems](mods/sef_ai_expansion) increases the AI's priority for claiming empty systems
surrounded by its own territory, helping it fill gaps and dead ends instead of leaving them
unclaimed. This is a normal defines mod and can be used without the DLL.

### Gigastructures: My Preset

[Gigastructures: My Preset](mods/sef_giga_user_preset) adds a **Save Settings as My Preset** edict
that records your current Gigastructural Engineering settings. During a later game's initial Gigastructures setup, choose **Presets → User Preset →
Enable Preset** to load them. Saving a preset again replaces the previous one.

It requires Gigastructural Engineering, this DLL and `[script_storage] enabled=1`, and should
load after Gigastructural Engineering in the launcher. Preset loading is available in single
player only because replaying stored script is disabled in multiplayer.

### Install an included mod

1. Copy the chosen mod folder from the ZIP's `mods` folder into
   `Documents\Paradox Interactive\Stellaris\mod\`.
2. Copy that folder's `descriptor.mod` into the parent `mod` directory and rename the copy
   to match the folder, for example `sef_ai_expansion.mod` or `sef_giga_user_preset.mod`.
3. Open the renamed copy in a text editor and add the matching path line:

   ```text
   path="mod/sef_ai_expansion"
   ```

   For the preset mod, use `path="mod/sef_giga_user_preset"` instead. Leave the original
   `descriptor.mod` inside the mod folder in place.
4. Restart the launcher, add the mod to your playset and enable it. For the preset mod, also
   enable Gigastructural Engineering and place the preset mod after it.
