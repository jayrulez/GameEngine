# Root motion - clips that carry their own travel

> STATUS: PROPOSED 2026-10-05 for review (the user, and the Sedulous session, which mirrors it).
> Seeded in weekly_backlog.md (2026-08-26). User ruling 2026-10-05: after Snowline's P1, before
> its P2, spec first. Companion: inverse-kinematics.md (P0 there, the space and the pose seam, is
> this spec's prerequisite). Read CONVENTIONS.md first.

## Why

A walk clip today plays in place, and a script moves the entity and guesses the clip's speed to
keep the feet from sliding (PaperKid's pedestrians and animals scale the Walk clip by
`pace / walkMetres`). With root motion the clip's own travel moves the entity: feet that never
slide, turns and starts and stops that move as they were animated.

## What the engine does today

- The importer drops channels on a non-joint node (the Blender armature object) and its
  transform; travel animated there is lost (fixed in inverse-kinematics.md P0).
- `AnimationClipSource` has no version field; a clip carries tracks per bone and nothing about
  its root.
- Sampling wraps time for a looping clip; nothing measures what a bone travelled between two
  times.

## Design

### Authoring (per clip)

- `AnimationClipSource` gains a data version and root motion settings: `rootBone` (a name; empty
  = the skeleton's first root), and what to extract: `horizontal` (the ground-plane translation),
  `vertical` (height: off for a walk, on for a climb), `yaw` (turning about up; no pitch or roll
  ever: summing them across blends is wrong, as ezEngine's comments admit).
- Set on the clip asset (the clip page's checkboxes; `asset_data_write` for agents), and at
  import (a Model importer option, "Root motion from the root bone").

### Cook (Lumix's arrangement)

- At cook, the root bone's motion curve is baked (its model-space translation and yaw at each key
  time), and the extracted components are STRIPPED from the root's track, keeping its frame-0
  offset, so the pose plays in place exactly as much as the clip says (a walk keeps its bob when
  `vertical` is off).

### Runtime

- **Delta between two clip times**: `inv(M(t0)) * M(t1)`, in the root's frame, split at the loop
  wrap into `(t0 -> end) * (start -> t1)`, and a step that spans whole loops adds the full loop's
  delta once per loop (Lumix loses those).
- **Players**: `AnimationPlayer` and every graph node that samples (clip, blend 1D, blend 2D)
  produce their delta for the frame; blend trees and crossfades blend DELTAS by the same weights
  they blend poses (translation lerped, yaw slerped about up); only the base layer contributes
  (an override or additive layer moves bones, not the character). Each player exposes
  `ConsumeRootMotion() -> {translation, yaw}` (model space), reset as it is read.
- **Applied** by the animator component's `rootMotion` mode:
  - `Ignore` (default: no behaviour changes for existing projects);
  - `Entity`: the delta, turned into world space by the animator's owner's rotation, moves and
    turns the owner (for a non-physics actor: PaperKid's dog and cat);
  - `Character`: the nearest ancestor (or self) with a `CharacterComponent` gets it as velocity
    (`delta / dt`, horizontal, through `move` or `drive` keeping gravity) and its yaw as rotation,
    so the controller still collides and slides;
  - `Script`: nothing applied; `SceneAnimation.rootMotion(entity)` reads the frame's delta for a
    script to use (a navigation agent that wants the clip's speed).
- **Frame order**: extracted in PostUpdate with the pose; applied there to the entity (Entity), or
  stored for the next fixed step (Character), which is where a character's velocity is consumed.

### Editor

- The clip page shows the extracted path (a ground line under the preview) and plays the stripped
  pose in place, with a toggle to see it travel.
- The graph page's preview can travel (root motion applied to the preview's root).

## Phases

- **P0** The armature node and its channels kept (inverse-kinematics.md P0); the clip's data
  version and root settings; the cook's bake and strip. Tests: a clip whose root walks 2 m bakes a
  2 m curve and plays in place; `vertical` off keeps the bob; an old clip (no version) loads.
- **P1** Deltas: the player's delta with the wrap split and whole loops; graph nodes, blend trees
  and crossfades blending deltas; base layer only. Tests: a looping clip's deltas over many
  frames sum to the authored travel per loop (no loss at the wrap, none for a step of two loops); a
  50/50 blend of two walks moves at the mean speed; a crossfade moves smoothly (no step at either
  end); yaw accumulates a turn clip's authored turn.
- **P2** Applying: the modes, the frame order, the script read. Tests: an Entity-mode walker
  covers the clip's distance over N loops within a tolerance and turns with a turn clip; a
  Character-mode walker collides with a wall instead of walking through it; Ignore leaves the
  entity where it was.
- **P3** Editor views; the proof: PaperKid's dog and cat walk by their clips' root motion (Pet.as
  steers by turning, the clip moves them) and their Blender clips are re-authored to travel; the
  pedestrians stay on their navigation agents (Script mode reads the clip's speed). Mirrored to
  Sedulous phase by phase.

## Open questions for review

1. Extract at cook (Lumix: a baked curve, stripped pose) or at runtime from the pose (LunarSong:
   the root bone's frame-to-frame delta)? This spec says cook: exact at the wrap, cheap at runtime.
2. Deltas from every sampling node (accurate through any blend) or from the dominant state only
   (simpler)? This spec says every node, blended like poses.
3. Character mode by velocity through the controller (collides, slides) or by teleporting the
   transform (exact, walks through walls)? This spec says velocity.
