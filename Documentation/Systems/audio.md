# Audio

> Status: CURRENT
> Verified: 2026-08-12 @ 51b22d7f
> Track: [[audio-subsystem]]

miniaudio-backed audio in the value-pool component style: clips + sound cues as cooked assets,
a data-driven bus mixer with effect chains, real 3D spatialization (attenuation / pan / doppler /
distance low-pass), listener-driven reverb zones, and a Wren/AngelScript facade. Shipped end to end
(P1-P3) and user-verified on speakers. miniaudio types never leak above `foundation.audio`; there is
no second-backend abstraction seam - what IS abstracted is a Null/headless mode (see below).

## Modules

- **`foundation.audio`** (`Code/Foundation/Audio`) - the miniaudio wrapper: `AudioEngine`, the voice
  pool + `VoiceStatus`, spatializer config, the `AudioBus` topology + `AudioBusEffectKind` chains,
  `Freeverb` (`AudioReverb`), `SoundCue`, and the `ma_vfs` bridge to `foundation.vfs` (streaming out
  of paks). Includes a Null device mode so headless/cook/CI never touch audio hardware.
- **`foundation.audio.resource`** (`Code/Foundation/Audio.Resource`) - the cooked `AudioClip` +
  `BusLayout` resources + factories.
- **`audio.pipeline`** (`Code/Pipeline/Audio.Pipeline`) - `AudioClipAsset` + `SoundCue` asset +
  builders + the wav/ogg/mp3/flac file importer.
- **`engine.audio`** (`Code/Engine/Engine.Audio`) - `AudioSubsystem` + the components + scene
  integration + the `Audio` script facade + `AudioUserSettings`.
- **`editor.audio`** (`Code/Editor/Editor.Audio`) - `AudioClipPage`, `SoundCuePage` (real-resolution
  audition), `BusLayoutPage`.

## Bus mixer (data)

`AudioBus` is a fixed four-bus topology: `Master` <- {`Effects` (SFX), `Music`, `UI`}. `BusLayout` is
the DATA layer over it: per-bus volume / mute / effect chains, cooked and referenced by a project
`defaultBusLayoutId` (manifest), applied by the player AND the Game tab. Effect kinds
(`AudioBusEffectKind`): Lowpass, Highpass, Delay, Reverb (Freeverb tail). Named custom bus trees are
additive over the fixed four (name + parent slots on the layout, cycle-rejecting cook, reconcile-by-
name on apply so voices survive); `busName` addressing works on params / components / script strings.
Music is a streamed clip routed through the graph like everything else (no stream-bypass), with
cross-fade.

## Voices

A fixed voice pool (`{slot, generation}` handles, no allocation after init). Stealing on exhaustion
(free -> lowest priority -> farthest same-priority) + recent-play dedupe. Stop/pause always FADES
(~10 ms, via miniaudio native fades; a bounded dying-voice arena handles faded steal since `ma_sound`
is not movable, with `DyingVoiceCount` observability). All engine API is on the main thread; miniaudio
owns the device/mix thread. `VoiceStatus.cursorSeconds` gives a true playhead (both editor pages use
it).

## 3D + reverb

Per source: attenuation model (inverse/linear/exponential), min/max distance, cone, doppler (fed
per-frame velocity from transform deltas), and a distance low-pass (`ma_lpf` per 3D voice, cutoff
driven by distance). Stereo-into-3D downmixes with a warn-once. Multi-listener: up to 4 spatial
listeners (closest-pick), for split-screen. Reverb: `AudioReverbZoneComponent` drives listener-
following environmental reverb on the scene's send Freeverb (the wettest overlapping zone wins,
crossfading across the edge band); `AudioReverbParams.dry` disambiguates insert vs aux-send, and
per-voice reverb sends route through a splitter to a wet-only second per-scene Freeverb.

## Resources + cooking

- **`AudioClip`** (cooked) - metadata (channels, rate, duration, loop points, gain, `stream`,
  `keepCompressed`) + the ORIGINAL compressed container bytes as the data stream (miniaudio decodes
  wav/flac/mp3/vorbis natively; no PCM sidecar bloat). `stream=true` decodes on the fly from the
  pak-backed VFS.
- **`SoundCue`** (cooked) - weighted variants + pitch/volume randomization; a pure data model
  (Traktor grain banks stay the north star - see backlog). Playable as a one-shot or on an
  `AudioSourceComponent`, with an inspector picker and a real-resolution audition page.
- **`BusLayout`** (cooked) - the serialized bus tree + effect chains.
- All resolve through `resource::Ref` + the scene resolve pass; the player needs zero cooking code.

## Scene integration + facade

- **`AudioSourceComponent`** - clip/cue ref, bus (name), volume, pitch, loop, spatial, autoPlay,
  priority, distance + cone + doppler fields; runtime `Play/Stop/Pause/SetPaused` + one-shot helpers.
  The component's volume and pitch are read when the source starts; `SetVolume`/`SetPitch`
  (script: `SceneAudio.setVolume(entity, volume, seconds)` / `setPitch`) ease a playing source's
  voice onto new values and keep them for its next play (a wind rising with speed).
- **`AudioListenerComponent`** - first active wins; falls back to the active camera transform.
- **Manager tick** (`ScenePhase::PostTransform`) resolves dirty refs, syncs position/forward/velocity,
  autoplays on simulation start, reaps finished one-shots; scene pause pauses that scene's voices
  (a per-scene group under the bus).
- **Runs** - everything one running game plays (a `GameInstance`: the player's, each Game tab's) is
  a run group above its scenes' groups, keyed by the run (`Scene::Run`, the script binding's
  `CurrentRun`). A run has its own music slot and its own bus gains; the subsystem ends a run
  (`EndRun`: a short fade, then freed), pauses it with the host (`SetRunPaused`), and hears only the
  focused run (`SetFocusedRun`; `SetHearAllRuns` for every instance), muting the rest while they keep
  running. Only heard runs' scenes feed the listeners.
- **`Audio` facade** (registered via the reflection facade path; AngelScript + Luau), as Sedulous's:
  `playOneShot(clip[, bus, volume, pitch])` / `playOneShot3D(clip, position)` / `playCue(cue[, bus])`
  / `playCue3D(cue, position)` / `playMusic(clip[, crossFadeSeconds, volume])` by ASSET ID, each
  returning the `VoiceHandle` (`isValid()` false when nothing played), and `setBusVolume` /
  `busVolume` by `AudioBus`. Ours beside them, named apart (a same-arity overload cannot share a
  name): `playOneShotPath` / `playOneShot3DPath` / `playCuePath` / `playMusicPath` by CONTENT PATH,
  `setNamedBusVolume` / `namedBusVolume` by bus name (a layout's custom buses too), `setBusMuted` /
  `busMuted` / `stopMusic`, and control of a PLAYING voice (what a play returned, or the run's
  `musicVoice()`): `setVoicePitch(voice, pitch[, seconds])` (the playback rate, tempo and pitch
  together: a game's hurry-up music), `setVoiceVolume(voice, volume[, seconds])`, both eased over
  `seconds` by the engine (smoothstep, held while paused), `stopVoice(voice[, fadeSeconds])`,
  `setVoicePaused`, `isVoicePlaying`; a `VoiceHandle` is constructible, so a script keeps one in a
  variable or member. Stale handles are inert. Bad content warns once and no-ops. In a run, what a script plays goes
  into the run (its Stop ends it) and the bus volumes and mutes are the run's own, a layout's custom
  named buses included (a game's sliders never move the editor's buses or another instance's). Master/bus volumes persist via
  `AudioUserSettings` (`<userdata>/<project>.user.settings.xml`).

## Deferred

Grain banks (Traktor's compositional sound-graph) and their incremental growth path from `SoundCue`
(in-loop-out, a parameter system, blend cues, sequence/composite cues):
`Documentation/Archive/audio-followups-history.md` (open items: week-2026-09-05).

---

Design rationale (the reference survey - Sedulous / Godot / Traktor / Lumix - the no-abstraction-
theater + Null-mode calls, the open questions and how they resolved) is in
`Documentation/Archive/audio-design-history.md`.
