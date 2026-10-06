# Save data - a game keeps what it learns between runs

## The problem

A game cannot keep anything between runs. The settings library (`foundation.settings`) is a
typed, versioned store, and the player uses it for one thing: the audio bus volumes, read at
startup from `<user data>/<project>.user.settings.xml` and written back at shutdown
(`PlayerApplication.h`). Nothing reaches a script:

- No script API saves or loads a value, so a best time, a high score, an unlocked level or a
  game's own option is lost when the game closes.
- The one file the player writes is written at shutdown only, straight over the old one: a crash
  or a forced quit loses the changes, and a crash mid-write leaves a torn file.
- Play in editor keeps nothing, and if it did it would need to stay out of the player's file.

## Proposal

### 1. A crash-safe whole-file write (Core)

`core::WriteFileAtomic(path, bytes)` writes `<path>.tmp` beside the target and renames it over
the target. Both platform renames replace an existing file (`rename`, `MoveFileExA` with
`MOVEFILE_REPLACE_EXISTING`), and on one volume the rename is atomic: a reader sees the old file
or the new one, never half of either. A failed write leaves the old file untouched.

### 2. A section of keyed values (Settings)

`settings::SaveValues` is a settings section whose fields are not known ahead: a map from a key
to a typed value (bool, int, float or text). It serializes as an array of `{key, kind, value}`,
so a save file is an ordinary settings file (the same envelope, versioning and unknown-section
passthrough) holding one `SaveValues` section. A value read as another kind than it was written
answers the reader's fallback, except that an int reads as a float (a whole number is a number).

A value can also be a list of numbers (kind `floats`, added 2026-10-05 for Snowline's ghost, a
recorded run of a few thousand numbers): written as a plain array in `value`, read back whole,
and an empty list when absent or another kind. Kinds are written by name, and a kind a build does
not know (a newer build's) skips that one entry: the rest of the save reads, and the next write
drops it. The skip is exact in a keyed format, which a save file is (XML); a positional payload
cannot size a value it does not know. Builds from before the skip fail the whole section on an
unknown kind, so they cannot read a save holding a list.

### 3. A run's save (Engine.GameInstance)

A `GameInstance` owns its run's save: the values, the file they live in, and whether they have
changed since the last write.

- `SetSaveFile(path)` names the file and loads it (absent is an empty save; unreadable is an
  empty save and a warning, and the file is left alone until the game writes).
- The run's script contexts get the save as a per-context service, as they get `run` and `Input`.
- `StopScript` writes the save if it changed, so a game that never asks still keeps its values
  when it ends cleanly. Writing at the moment that matters (a level clear) is the game's call.

### 4. The `Save` facade (scripts)

A static facade, the same shape on both backends:

```
Save::setInt("best.level2", 4210);      // setFloat / setBool / setString
int best = Save::getInt("best.level2", 0); // getFloat / getBool / getString, with a fallback
Save::has("best.level2");  Save::remove("best.level2");  Save::clear();
Save::flush();             // write now if anything changed; false if the write failed
Save::setFloats("ghost.meadow", samples); // a list of numbers (array<float>, a Luau table)
array<float>@ run = Save::getFloats("ghost.meadow"); // empty when absent or another kind
```

A context with no run save (an editor tool) reads every fallback and writes nowhere.

### 5. Where each host keeps it

- **Player:** `<user data>/<project>.save.xml`, beside the user settings file.
- **Play in editor:** `<project>/Editor/<project>.save.xml`. `Editor/` is the project's ignored
  per-user state, so testing never touches a player's save and nothing lands in source control.
  Several Game tabs of one project share the file; the last to write wins.

## Not now

- A web backend: the web build's user data directory is in-memory, so a save lasts one page load.
  Browser storage (IndexedDB) is the follow-up.
- Save slots, and saving a scene's state: the keyed values are what the sample games need.
- Writing the audio volumes through the same atomic path at the moment they change; they still
  write at shutdown.
- An editor or MCP control to inspect or clear the play-in-editor save.
