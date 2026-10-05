# Inverse kinematics - bones that reach, look and stand on the ground

> STATUS: APPROVED 2026-10-05 by the user (the proposals as written, open questions taken as the
> spec answers them); reviewed by the Sedulous session the same day (its points folded
> in: the root correction per root, P0 split, model space with several meshes, instanced skinning,
> how a component finds its animator, the hinge fallback, the bone-name attribute).
> P0b BUILT 2026-10-05: the pose modifier stage (`:modifier` - ModelPoseCache, IPoseModifier,
> PoseModifierStack) in both players, between the pose and the palette.
> P0a BUILT 2026-10-05: decided for the importer's prefab, no skeleton correction (below).
> P1 BUILT 2026-10-05: `:ik` (SolveTwoBone, SolveAim) with the solver tests below. Two
> refinements of the text: bone lengths are the pose's own, and the aim's pole is an up direction.
> P2 BUILT 2026-10-05: TwoBoneIkComponent and AimIkComponent (`engine.animation:ik`), their
> managers before the animation managers, fades, order, the three script calls, debugDraw while a
> scene runs. Two notes: world matrices are cached only after PostUpdate, so the managers compose
> the targets' and the mesh's worlds fresh; the aim's `up` is an entity the up axis leans toward.
> The editor's gizmo for a selected IK component moves to P4 with the bone pickers.
> P3 BUILT 2026-10-05: SolveFootIk (Foundation, given grounds) and FootIkComponent, probing
> through a new scene seam, ISceneRayQuery (SceneSystem::AsRayQuery; PhysicsSceneSystem answers,
> triggers skipped), so animation needs no physics link. Folded in from Sedulous's review: one
> probe and one easing step a frame (a second evaluation reuses the hits; a zero step holds),
> `groundHeight` for a rig whose origin is not at its feet, the order pelvis, rebuild, legs. The
> camera cull distance is deferred (no main camera at this layer; a script-set viewer point later).
> P4 (editor) BUILT 2026-10-05: bone pickers (`boneName` lists the animator's bones; a missing name
> is listed as such) and gizmos for the three IK components. Sky Hopper's hero (an asset-pack rig)
> has DETACHED feet: IK-target bones off the root, the shin with no child. SolveTwoBone now takes
> a detached end (any chain, so a hand on such a rig too): the end bone moves to the target itself
> (blended by the weight, turned with matchRotation) and the chain bends all the way to meet it
> with its TIP, the point of the mid bone that met the end. The tip is taken from the animated pose
> every solve (foot IK takes it before the pelvis moves), never from the bind pose: it keeps any
> gap the baked rig left between the shin and the foot instead of snapping it shut. An end the
> start carries but the mid does not, or one above the chain, is refused (no meeting point).
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
  P0a finds out what that costs on real assets before anything changes (it is root-motion.md's
  prerequisite too). The correction is PER ROOT (Sedulous keeps it on each root bone): a skin with
  two roots under different non-joint parents needs two.
- **Risk to settle first**: in a Blender glTF the armature node parents both the skinned mesh and
  the joints. If the prefab keeps the armature as an entity, the mesh entity's world may ALREADY
  include its transform, which may be why characters draw right today; restoring the correction
  would then apply it twice. P0a checks the real assets (Sky Hopper's hero, PaperKid's kid,
  pedestrian and animals, Snowline's rider) in both engines, before and after, and decides whether
  the fix belongs in the skeleton or in the importer's prefab.
- **P0a, decided**: measured on all nine animated sample models (Snowline's rider; PaperKid's
  kid, dog, cat and pedestrian; Sky Hopper's crab, skull, bee and hero with four meshes): each
  is prefab root, armature entity, skinned mesh entity, all at identity. The skin is drawn as
  `vertex * inverseBind * jointChain * meshWorld`, correct exactly when the mesh's world is the
  world of the node the skeleton hangs from; a skeleton correction would apply the armature
  twice. So: the manifest records `skeletonParentNode` (the parent of the skin's root joint;
  data version 3, a v2 manifest still reads and keeps the file's placement), and the prefab puts
  each skinned mesh entity under that node at identity (glTF ignores a skinned mesh node's own
  transform). Model space is that node's world, reached through the mesh entity as before.
  Channels animated on the armature node itself are still dropped: keeping them changes the
  clip's data, which root-motion.md P0 does anyway, so they move there.
- **Model space with several meshes**: an animator feeds one palette to each of its mesh
  entities, and each draws at its own world. Model space is defined as the FIRST resolved mesh
  entity's world (the animator's own entity when it lists none); the mesh entities of one
  animator must share a world transform (they do for an imported model: siblings under the rig),
  which the editor warns about when they do not.
- **Instanced skinning (crowds)** shares pose phases between instances: IK does not apply there.

## Design

### Foundation: the solvers (`foundation.animation:ik`)

Pure functions over a `Skeleton` and a local pose (`Span<BoneTransform>`), with a reusable scratch
of model-space transforms; no physics, no scene.

- **TwoBone** (arm, leg): start, mid and end bone indices; a model-space target position; an
  optional target orientation for the end bone; a pole (model-space point) or, without one, the
  plane of the animated mid joint (traktor's choice: feet need no pole). When the animated chain
  is straight, the plane of the BIND pose's own bend (usually slightly bent); when that is straight
  too, a hinge axis the caller gives (a knee and an elbow bend opposite ways, so no fixed default);
  weight 0..1. Law of cosines with the POSE's bone lengths (a rotation keeps them, so a bind
  length would miss whenever an animation moved a bone), reach clamped to
  `[|l1-l2|, 0.995 (l1+l2)]`, the upper bound raised to the pose's own span when the animation
  already holds the chain straighter (P3 found a standing leg pulled up 4 mm otherwise). Two steps: the mid joint turns about the hinge (the bend plane's
  normal) to span the distance, then the start swings the end onto the target and the bend side
  onto the pole's (or carried with the swing), so the mid joint stays in its plane. Writes the start and
  mid LOCAL rotations (a correction quaternion in model space, converted through the parent's
  model inverse) and, with an orientation, the end's. Result: reached, and the residual distance.
- **Aim** (head, spine, a weapon): a list of bones with per-bone weights (a spine shares the turn:
  0.3, 0.5, 1.0), an aim axis and an up axis in the last bone's space, a model-space target, a
  maximum angle from the animated direction (60 degrees default) and weight. Each bone takes its
  share of the remaining swing, then the last bone rolls about its aim so the up axis leans
  toward an up direction (model space), or back to the animated up without one (no roll drift).
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

- **Finding the animator**: a component drives the nearest ancestor-or-self with a skeletal
  animation or animation graph component, else (P4) the ONE below it, so IK can sit on a gameplay
  root whose model is a child (an importer's prefab the game does not edit). Several below is
  ambiguous (a pet, a held prop): reordering children must never retarget the IK silently, so the
  component stays off with one log line, as it does with none.
- **`TwoBoneIkComponent`**, **`AimIkComponent`**, on an entity under the animated model (or on it):
  the chain by BONE NAMES (resolved to indices against the animator's skeleton, cached, re-resolved
  when the skeleton changes; an unknown name logs once and disables the component), the target an
  `EntityRef` (its world position, and rotation if `matchRotation`) or, empty, the component's own
  entity; pole an `EntityRef` (optional); `weight` and `fadeSeconds` (the weight eases toward
  `active ? weight : 0`); `order` (lower first; ez's ordering, so a look-at runs after the legs).
- **`FootIkComponent`** (as built): the animation's ground is the plane through the model origin
  across up, offset by `groundHeight` (0 for every imported sample rig: P0a measured their origins
  at the floor). A planted foot moves by the ground's height under it; the pelvis lowers by the
  deepest weighted correction, clamped to `pelvisDropMax`; then each leg is a two-bone solve with
  the foot turned to the slope within `maxTilt`. Corrections ease with `1 - exp(-rate dt)`, faster
  rising than lowering. The probe: one ray per foot a frame, from above the animated foot down,
  through ISceneRayQuery (solid surfaces only).
- **`FootIkComponent`** (as proposed): two (or more) legs by bone names, the pelvis bone, ray settings (up and
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
- `SceneAnimation.setIkTarget(entity, Float3)` for a component without a target entity, and
  `ikReached(entity)` (bool) / `ikError(entity)` (float) to read the result: no new script types
  (both engines keep a tripwire on the bound-type count). Sedulous spells them PascalCase on
  `scene.Animation`.

### Editor

- Bone fields pick from the animator's skeleton (a list of bone names), not free text: the field
  carries a `boneName` attribute (the same name in both engines) that the inspector turns into
  that picker.
- The inspector shows a component whose chain does not resolve as an error row.
- The animation graph page's preview runs the modifiers of the selected entity's IK components
  (P3).

## Phases

- **P0a Space.** Measure the real assets before and after keeping the armature: where the mesh
  entity's world comes from, whether the correction double-applies; decide skeleton (a per-root
  correction, serialized in `SkeletonSource`) or importer prefab; armature-level channels kept
  (root-motion.md's need); the cooked data's version change and a re-cook of the sample projects
  (Integration.Mcp checks their data versions). Tests: a skeleton under a moved and rotated
  armature skins where the source file draws it, exactly once; the sample models draw unchanged.
- **P0b Seam** (independent of P0a, may land first): the players' modifier stage and
  `ModelPoseCache`. Tests: a modifier's edit survives an evaluation; one model-space build per
  evaluation.
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
