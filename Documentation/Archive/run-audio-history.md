# Run audio - a game's sound belongs to its run

> ARCHIVED 2026-11-02, fully built; anything it left open is in
> Documentation/Plans/weekly_backlog.md ("Left over from the specs archived 2026-11-02"). Non-
> authoritative: the original spec, kept as the record of what was built and why; the code and the
> tests are the truth.

## The problem

Stopping a game in the editor does not stop its music. The user closes the editor to silence it.

- There is one `AudioEngine`, owned by the one `AudioSubsystem`, shared by everything in the
  process: the editor and every Game tab (`AudioSubsystem.cppm:12`, "The subsystem owns the ONE
  AudioEngine").
- A scene's own sound sources are already scoped: each scene gets a voice group when it starts
  and destroys it when it stops (`AudioSubsystem.cppm:147`, :188), and a paused simulation pauses
  the group (:254).
- What a game plays from script is not scoped at all. `Audio.playMusic` and `Audio.playOneShot`
  go to the engine directly (`AudioSubsystem.cppm:1076`, :1132); music "carries no scene group,
  so it survives scene swaps" (`AudioEngine.cppm:354`). Nothing owns those voices, so nothing
  stops them when the run ends: the music plays on after Stop, and so does a jingle or a voice
  line that was mid-way.
- The script binding is one per subsystem (`ExposeToScript` sets the subsystem's single
  `m_scriptBinding`), configured for every script context app-wide
  (`DefaultApplicationImpl.cpp:263`), so a context cannot tell which run it belongs to.

Multi-PIE (Play New Instance) makes it worse:

- Every instance's sound mixes into the one output: two copies of the music, each instance's
  effects on top of the other's.
- Every scene's listeners are enabled at once (`AudioSubsystemImpl.cpp:86`), so 3D sound is heard
  from several places.
- Bus volumes are global: one instance's Settings slider changes the other instance's volume,
  and the editor's own.

Stopping all audio on Stop is not the answer: in multi-PIE it would silence the instances still
running. But two instances heard at once is not a good experience either, so multi-PIE needs a
rule for which one is heard.

## Proposal

### 1. A run group above the scene groups

The engine's graph already has a per-scene child group under each bus. Add one level above it:
a **run group** per run (a `GameInstance`: the player's one, each Game tab's).

- Scene groups are created as children of their run's group.
- A run group has its own music slot: `PlayMusic` / `StopMusic` take the run, so two runs never
  cross-fade each other's music.
- One-shots, cues and music a run's scripts start go into that run's group.
- Stop, pause, mute and volume act on the whole run: `StopRunGroup` (a short fade, then free),
  `SetRunGroupPaused`, `SetRunGroupMuted`, `SetRunGroupVolume`.

The editor's own sounds (asset previews) stay outside every run.

### 2. The run's script binding knows its run

`GameInstance` installs the audio binding for its contexts, as it already installs its net
binding, carrying its run group. The `Audio` facade plays into `binding->runGroup`; a context with
no run (an editor tool) plays into the global group as today.

### 3. What each host does with it

- **Stop (Game tab):** stop the run's group. Its music, one-shots and sources end with a short
  fade, whatever else is running. This alone fixes the reported problem.
- **Pause (the toolbar's pause, a debugger break):** pause the run's group, so music and one-shots
  freeze with the game rather than playing over a paused frame. Time scale 0 from a game's own
  pause menu stays the game's business: its menu music should keep playing.
- **Player:** one run, so nothing changes, except that exiting stops the run cleanly.

### 4. Multi-PIE: one instance is heard

Only the **focused** Game tab's run is audible; the others keep running with their run groups
muted, not paused, so their timelines and state stay true. Clicking into another Game tab moves
the sound to it (a short cross-fade). Only the audible run's listeners feed the engine, so 3D
sound is heard from one place.

An editor setting, **Game audio: focused instance | all instances**, keeps the old behaviour for
the rare test that wants every instance heard (default: focused).

### 5. Bus volumes in play-in-editor

A game's Settings sliders set the global buses, so in multi-PIE one instance changes the others,
and in any PIE it changes the editor's own volume and the volumes the player saves. Options:

- (a) Per-run bus gains under each run group: a run's `setBusVolume` writes its own gains; the
  player still saves them; PIE never touches the editor's buses.
- (b) Leave buses global and accept it, since only the focused instance is heard anyway.

Decided (user, 2026-10-03): **(a)**, per-run bus gains. Not to start until Sedulous has finished
its sync, so no code changes under it.

## Tests

- A run's music and one-shots stop when its run group stops; another run's do not.
- A scene's group is a child of its run's: stopping the run stops the scene's sources.
- Two runs' music slots are independent (a cross-fade in one leaves the other playing).
- Muting a run silences it and keeps its voices advancing; unmuting resumes at the right place.
- A script context bound to a run plays into that run's group; one with no run plays globally.
- Game tab: Stop silences the run (Integration, the embedded host); with two tabs, focus decides
  which is heard.

## Not in scope

Separate audio devices per instance; mixing several instances into one output deliberately;
networked audio.

## Built (run-audio branch)

- Audio engine: run groups (`CreateRunGroup`, `StopRunGroup`, `DestroyRunGroup`, pause, mute with a
  0.1 s ramp, volume, per-run bus gains and mutes, a music slot per run); scene groups nest under
  their run; a scene-less voice can name its run. A layout's custom buses get the same children:
  a run (and a scene) has a child group under each custom bus it plays on, so a named-bus voice
  routes scene child -> run child -> bus and the run's gains, mute and pause reach it through the
  graph; a run's gain for a named bus is its own (`SetRunNamedBusVolume`).
- The run key: `Scene::Run` (set by the creating manager) and the script runtime binding's `run`
  (`CurrentRun`); a `GameInstance` gives both itself.
- Audio subsystem: a run registry keyed by run (`RunGroupFor`, `EndRun` with a fade then free,
  `SetRunPaused`, `SetFocusedRun`, `SetHearAllRuns`); scenes of a run start their group under it;
  the `Audio` facade plays into the calling script's run and its bus volumes and mutes are the
  run's; only heard runs' scenes feed the listeners.
- Hosts: the Game tab ends its run on Stop, keeps its run paused with the toolbar's pause and a
  debugger break, focuses its run on Play and when its viewport takes the keyboard; the app ends
  a released instance's run; Preferences > Game audio > "Hear every Game tab" (default off).
