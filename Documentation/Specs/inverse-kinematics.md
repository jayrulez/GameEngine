# Inverse kinematics - bones that reach, look and stand on the ground

> STATUS: PROPOSED 2026-10-05 for review (the user, and the Sedulous session, which mirrors it).
> Seeded in weekly_backlog.md (2026-08-26). User ruling 2026-10-05: after Snowline's P1, before
> its P2, spec first, and "when doing IK, it must be solid". Companion: root-motion.md (the two
> share the pose seam of P0). Read CONVENTIONS.md first.

## Why, and what "solid" means here

Nothing in the engine can bend a pose toward a target: a hand cannot reach a handle, a head
cannot follow what it looks at, a foot floats over a slope or sinks into a step. Skeletal
animation plays clips and graphs; nothing modifies the result.

Solid, as this spec holds it:
- **Exact where it can be**: an analytic two-bone solve reaches a reachable target exactly, and an
  unreachable one is clamped to the chain's full reach (short of locking straight), never NaN,
  never a flipped knee.
- **Stable**: the bend plane is defined (a pole, or the animated mid joint), so a joint never rolls
  or flips between frames; a straight chain has a fallback.
- **Blendable**: every solve has a weight that fades in and out over time, so IK never snaps on or
  off.
- **Space-correct**: one documented space (the skeleton's model space, anchored where the skinned
  mesh is drawn), targets converted into it once, and tested.
- **Wired end to end**: solvers, the engine's components, scripts, debug drawing and the editor,
  each with tests; a solver nothing reaches does not count.
- **Cheap**: no allocation per frame after the first; cost proportional to the chains solved.

## Where it plugs in (the engine today)

- Poses are LOCAL (parent-relative) `BoneTransform`s; `Skeleton::ComputeWorldPoses` builds model
  space (row-vector math, `child = local * parentWorld`), `ComputeSkinningMatrices` the palette.
- `AnimationPlayer::Evaluate` samples and builds the palette in one step: there is no seam, and
  `SetBonePose` is overwritten by the next sample. `AnimationGraphPlayer::Update` ends with
  `CombineLayers`, then the palette is built lazily.
- The engine's managers (`SkeletalAnimationComponentManager`, `AnimationGraphComponentManager`)
  run in PostUpdate: `player->Update(dt)`, then `GetSkinningMatrices()` into the mesh. Model space
  is the skinned mesh entity's world (`ExtractImpl`: `rd.world = GetWorldMatrix(meshEntity)`).
- The importer drops a non-joint parent of the skeleton (the Blender armature object) and
  `Skeleton::rootCorrection` is never set: model space silently excludes that node's transform.
  Fixed in P0 (it is root-motion.md's prerequisite too).

## Design

### Foundation: the solvers (`foundation.animation:ik`)

Pure functions over a `Skeleton` and a local pose (`Span<BoneTransform>`), with a reusable scratch
of model-space transforms; no physics, no scene.

- **TwoBone** (arm, leg): start, mid and end bone indices; a model-space target position; an
  optional target orientation for the end bone; a pole (model-space point) or, without one, the
  plane of the animated mid joint (traktor's choice: feet need no pole), falling back to a hinge
  axis when the chain is straight; weight 0..1. Law of cosines with BIND-pose bone lengths (an
  animation may stretch them), reach clamped to `[|l1-l2|, 0.995 (l1+l2)]`. Writes the start and
  mid LOCAL rotations (a correction quaternion in model space, converted through the parent's
  model inverse) and, with an orientation, the end's. Result: reached, and the residual distance.
- **Aim** (head, spine, a weapon): a list of bones with per-bone weights (a spine shares the turn:
  0.3, 0.5, 1.0), an aim axis and an up axis in the last bone's space, a model-space target, a
  maximum angle from the animated direction (60 degrees default) and weight. Each bone takes its
  share of the remaining swing, then the up axis is kept toward the pole (no roll drift).
- **Rebuild**: after a solve, model space is rebuilt from the chain's start downward only (a
  correction lower in the hierarchy never moves a bone above it).
- **Order and weight are the caller's**: solvers do not fade; the engine owns time.

### Foundation: the pose seam (shared with root motion)

- `AnimationPlayer::Evaluate` splits into sample, modify, palette; `AnimationGraphPlayer` exposes
  the same after `CombineLayers`. A player holds a `PoseModifier` list (an interface:
  `Apply(const Skeleton&, Span<BoneTransform> local, ModelPoseCache&)`), run in their order
  every evaluation, so a modification survives the next sample and the palette sees it.
- `ModelPoseCache`: the model-space pose built once per evaluation and kept current by each
  modifier's partial rebuild, so N chains do not cost N full rebuilds.

### Engine: components and the frame

- **`TwoBoneIkComponent`**, **`AimIkComponent`**, on an entity under the animated model (or on it):
  the chain by BONE NAMES (resolved to indices against the animator's skeleton, cached, re-resolved
  when the skeleton changes; an unknown name logs once and disables the component), the target an
  `EntityRef` (its world position, and rotation if `matchRotation`) or, empty, the component's own
  entity; pole an `EntityRef` (optional); `weight` and `fadeSeconds` (the weight eases toward
  `active ? weight : 0`); `order` (lower first; ez's ordering, so a look-at runs after the legs).
- **`FootIkComponent`**: two (or more) legs by bone names, the pelvis bone, ray settings (up and
  down range from the sole, collision mask), `pelvisDropMax`, `maxTilt` (30 degrees), smoothing
  rates (lifting a foot settles faster than lowering it), `liftHeight` (a foot the animation has
  lifted above it is swinging: its weight fades to 0), a cull distance with a fade band (traktor's
  recipe). The physics probe happens in the engine layer and feeds the solver: the Foundation
  solver stays physics-free (LunarSong's split).
- **The frame**: in the animation managers, after `player->Update(dt)` and before the palette:
  gather the animator's IK components in order, convert their world targets into model space with
  the inverse of the skinned mesh entity's world matrix, run the modifiers. Targets read this
  frame's transforms (PostUpdate runs after scripts and the physics step): no async staleness.
- **Debug draw**: each component draws its target (green reached, orange not), its pole and the
  chain, when the editor's gizmos are on or a run asks (`debugDraw`).
- **Bone attachments are out of scope** (bone entities are not written from the pose); noted as
  the natural next seed.

### Scripts

- The components are reflected: `TwoBoneIkComponent.of(e).weight`, `.target`, `active`.
- `SceneAnimation.setIkTarget(entity, x, y, z)` for a component without a target entity, and
  `ikReached(entity)` / `ikError(entity)` to read the result.

### Editor

- Bone fields pick from the animator's skeleton (a list of bone names), not free text.
- The inspector shows a component whose chain does not resolve as an error row.
- The animation graph page's preview runs the modifiers of the selected entity's IK components
  (P3).

## Phases

- **P0 Space and seam.** The importer keeps the skeleton's non-joint parent as the skeleton's
  root correction (and armature-level channels are not lost: root-motion.md's need), serialized;
  the players' modifier stage; `ModelPoseCache`. Tests: a skeleton under a moved and rotated
  armature skins where the source file draws it; a modifier's edit survives an evaluation; one
  model-space build per evaluation.
- **P1 Solvers.** TwoBone, Aim, Rebuild, in Foundation, with their tests (below).
- **P2 Components.** TwoBone and Aim components, the manager wiring, fades, order, script facade,
  debug draw. Tests: a scene where a target entity moves and the hand bone's world position follows
  it; weight fades over `fadeSeconds`; two components run in order; an unknown bone disables with
  one log line.
- **P3 Foot IK.** The component, the probe, smoothing, pelvis drop, tilt. Tests on a physics slope
  and a step: the planted foot sits on the surface within a tolerance, the pelvis drops by the
  deepest correction (clamped), a lifted foot is untouched.
- **P4 Editor and proof.** Bone pickers, the graph preview, and the proof in games: Sky Hopper's
  hero (feet on slopes and steps, look-at toward the next coin), Snowline's rider (head looks
  toward the next gate). Mirrored to Sedulous phase by phase.

## Tests the solvers must pass (P1)

- TwoBone reaches a reachable target exactly (within 1e-4 of chain length scale), for targets all
  round the chain; bone lengths are kept exactly; the mid joint lies in the pole's plane.
- An unreachable target: the chain points at it, stopped at 0.995 of full reach; no NaN for a
  target at the chain's start or in a degenerate line.
- No pole, straight animated chain: the hinge fallback bends it, the same way two frames running.
- Weight 0 leaves the pose untouched byte for byte; weight 0.5 is between.
- Aim reaches the target direction within the max angle and clamps beyond it; the up axis holds.
- A rebuild leaves bones above the chain unchanged.
- No allocation after the first call (an allocator that counts).

## Open questions for review

1. Components on entities under the model (ez's arrangement, targets as their own entities), or
   fields on the animator component? This spec says components: several chains per model, each
   with its own target, ordered.
2. A two-bone solve without a pole: the animated mid joint's plane (traktor) as the default, or
   require a pole? This spec defaults to the animated plane, pole optional.
3. FABRIK / CCD for longer chains (tails, tentacles): not in this spec; seed it when a game needs
   it.
