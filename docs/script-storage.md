# Script storage for mods

Script storage lets Paradox-script mods keep numeric values and recorded effects outside the
game's save files. Mods access it through `log = "@sef:..."` effects and triggers, with a
shared store for data used across campaigns and a separate store associated with each save.
The [README](../README.md#included-mods) describes included mods that use this facility, while
[Configuration](configuration.md) covers its settings.

`[script_storage] enabled=1` enables the facility, but a mod must issue storage commands before
it does anything. Without the DLL or with storage disabled, these remain ordinary game log
statements and perform no external storage operations. The feature has had limited testing;
multiplayer has not been tested at all and its experimental mode is disabled by default.

## Stores and persistence

Both stores live beneath `sefdata` beside the DLL in the game folder:

| Store prefix | File | Lifetime |
|---|---|---|
| `global.` | `sefdata\global.txt` | Shared across campaigns using this installation; loaded on first use. |
| `save.` | `sefdata\saves\<folder>\<name>.txt` | Associated with the corresponding save path; loaded when that save loads and cleared for a new game. |

Global changes are held in memory and normally flushed at most once per second by the frame
callback. They are also flushed when a save begins and, when possible, when the process exits.
This batching means a command returning does not imply that its value is already on disk.

The save store is captured when a save request is accepted and written after the game's save
write succeeds. Its path follows the save's folder and filename, with `.sav` replaced by `.txt`.
The implementation includes the synchronous Ironman save-on-exit path, although this does not
establish Ironman or achievement compatibility.

These files are separate from the `.sav` archive, so copying or renaming a game save does not
automatically carry its external data with it. Include the matching sidecar file when moving
or sharing data that depends on the save store, preserving the corresponding folder and name.
Steam or GOG save synchronization should not be assumed to include `sefdata`.

An absent store file means an empty store, and an empty store's file is removed when it is
written. Nonempty files use sorted `KEY = "VALUE"` lines with escapes for backslashes, quotes,
newlines, and carriage returns. Writes go through a temporary file and replacement. The
serialization and path rules are implemented in [src/pdxstore.h](../src/pdxstore.h), with the
save-completion hooks in [src/dllmain.cpp](../src/dllmain.cpp).

## Names, values, and scope

Keys and variable names contain 1–128 ASCII letters, digits, or characters from `_ . : @ -`.
They are case-sensitive. Store keys are shared within their store rather than automatically
separated by mod or game scope, so use a prefix such as `mymod.setting` to avoid collisions.

Numeric commands read or write variables in the current Paradox-script scope. The scope must
support variables, just as it would for the game's normal variable effects. A `set` source
can be a current-scope variable or a decimal literal; it does not evaluate an arbitrary script
expression. Decimal literals use ordinary notation without exponents, and fractional digits
after the fifth are discarded.

One stored value can contain at most 1 MiB, and the combined keys and values of each store can
contain at most 16 MiB. Recorded scripts are stored as text values, so `get` cannot read them
into a numeric variable. The experimental multiplayer snapshot has a smaller combined limit,
described below.

## Command reference

Every entry in the first column is placed inside `log = "@sef:..."`. Replace `global.` with
`save.` to use the save store; `record_end` has no store prefix because it closes the current
recording.

| Text after `@sef:` | Context | Behavior |
|---|---|---|
| `global.set KEY = VARIABLE` | Effect | Stores the current scope's numeric variable under `KEY`; an unset variable removes that key. |
| `global.set KEY = 12.5` | Effect | Stores the numeric literal under `KEY`. |
| `global.set VARIABLE` | Effect | Stores the current scope's variable under a key of the same name. |
| `global.get VARIABLE = KEY` | Effect | Copies the stored number into the current scope's variable; a missing key leaves the variable unchanged. |
| `global.get VARIABLE` | Effect | Reads the key with the same name as the destination variable. |
| `global.clear KEY` | Effect | Removes the key if it exists. |
| `global.has KEY` | Trigger | Returns whether a value is stored under the key. |
| `global.record KEY` | Effect | Begins recording processed game-log text into a text value. |
| `record_end` | Effect | Finishes the active recording and stores its text under the selected key. |
| `global.run KEY` | Effect | Queues stored text for execution through the console's `effect` command between ticks; available only in single player. |

Only `has` is valid in a trigger or `limit` block. Calling it as an effect, or calling another
storage command as a trigger, reports an error rather than performing the requested operation.
When multiplayer storage is unavailable or waiting for a host snapshot, `has` returns false
and storage effects do not proceed.

### Example: a counter shared across games

This effect initializes a local variable, reads the previous count when present, increments
it, and stores the result. Run it once at the point where your mod considers a new playthrough
to have started:

```text
immediate = {
    set_variable = { which = times_played value = 0 }
    log = "@sef:global.get times_played = mymod.times_played"
    change_variable = { which = times_played value = 1 }
    log = "@sef:global.set mymod.times_played = times_played"
}
```

A stored-key check can be used in a normal condition:

```text
if = {
    limit = {
        log = "@sef:global.has mymod.times_played"
    }
    log = "@sef:global.get times_played = mymod.times_played"
}
```

## Recording and running effects

Recording captures the text produced by the game's log effect after its text substitutions
have been resolved, including values printed through `\[...]`. It keeps trimmed lines that
contain `=`, `{`, or `}`, while dropping empty lines, comment lines, and headings without script
syntax. This is useful for a mod that already prints a settings preset as executable script.

For example, the following records a simple effect without executing it at that point:

```text
immediate = {
    log = "@sef:global.record mymod.preset"
    log = "set_variable = { which = mymod_setting value = 12.5 }"
    log = "@sef:record_end"
}
```

To request execution later, issue:

```text
log = "@sef:global.run mymod.preset"
```

Execution is queued for the main-thread callback between ticks. It uses the console effect
command's scope, which is the player's country or the object currently selected in the
interface, rather than retaining the scope that called `run` or originally recorded the text.
Recorded code must account for that scope explicitly, and the console effect hook must be
available for execution to work.

Only one recording may be active at a time. It must be finished on the same thread and within
the same frame; an unfinished recording is discarded at the frame callback. Each processed
log line has an 8191-byte capture limit, and longer lines are truncated with an error in the
DLL log. Keep recordings self-contained so unrelated log effects cannot become part of the
stored script.

`has` reports the presence of either a number or recorded text, so it does not establish that
the stored value is suitable for a particular command. `get` reports an error for text that
cannot be read as a number, while `run` passes the stored content to the game's effect parser.
If that parser rejects the effect, the DLL log directs you to the game's `error.log`.

## Experimental multiplayer storage

**Multiplayer has not been tested at all.** The implementation and offline harnesses described
here do not establish that live games, joins, or synchronization work correctly. The default
`[script_storage] multiplayer=0` disables storage operations in multiplayer.

With both `enabled=1` and `multiplayer=1`, the implementation is designed to send the host's
global and save stores to peers as a snapshot when starting a game or transferring the game
state to a joining player. Snapshots travel through ordered game chat commands that the DLL
consumes without displaying in chat. Each machine then runs the same script against its copy,
so later writes still depend on deterministic script execution; they are not individually
sent back to the host as storage requests.

Only the host reads and writes persistent store files. Clients use temporary in-memory copies,
and their local files do not supply the session's values. Storage stays unavailable until the
current session's complete snapshot has executed, including on the host, and resynchronization
temporarily suspends storage at an ordered command boundary.

The current limitations are:

- Both serialized stores, including snapshot framing, must fit within 1 MiB; a larger snapshot
  causes the host to post a storage-disable command for the session.
- `run` is disabled in multiplayer, including on the host.
- Host migration, localized recordings, and concurrent script writes are unsupported, because
  peers must retain the same values and execute changes in the same order.
- All peers need the same DLL and relevant settings. Enabling multiplayer storage contributes
  to the settings checksum when `[multiplayer] settings_in_checksum=1` and its hook is available;
  this mechanism has not been validated in a live multiplayer game.

See [Configuration](configuration.md) for checksum settings. The transport and snapshot format
are implemented in [src/mpstore.h](../src/mpstore.h), while the session callbacks and readiness
checks are in [src/dllmain.cpp](../src/dllmain.cpp).

## Diagnosing storage problems

Check the `script_storage` component in `stellaris-engine-fixes.log` for startup availability,
file read/write errors, malformed commands, missing variables or unsuitable scopes, and size
limits. Basic storage, save persistence, and recorded-script execution have separate hook
requirements, so successful initialization of one does not establish that the others are
available. Detailed command problems are limited to the first 100 reports per process, while
the statistics counter continues to record further problems.

The [development guide](development.md) describes the log format and offline validation. Its
script-store and multiplayer harness checks exercise serialization and callbacks in test
conditions, with live gameplay testing still required.
