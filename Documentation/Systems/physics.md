# Physics

> Status: CURRENT
> Verified: 2026-08-12 @ 33f64a02
> Track: [[physics-p1]]

Jolt-backed physics in the value-pool component style: rigid bodies, colliders, characters,
joints, cooked collision shapes, a designer collision-group matrix, fixed-timestep stepping with
render interpolation, and real queries + contact events. Shipped end to end (P0-P3) and wired into
`DefaultApplication`. Jolt types never leak above `foundation.physics`; there is no second-backend
abstraction seam (deliberate - see the design history).

## Modules

- **`foundation.physics`** (`Code/Foundation/Physics`) - the Jolt wrapper: `PhysicsWorld`,
  `ShapeKind`/`ShapeDesc`, bodies, queries, layer/group filtering, contact buffering, temp
  allocation, the job adapter. `ObjectLayer = (broadphaseLayer << 8) | (group & 0x1F)`.
- **`foundation.physics.resource`** (`Code/Foundation/Physics.Resource`) - `CollisionShape`, the
  cooked shape product (Jolt `SaveBinaryState` bytes) + its runtime factory.
- **`physics.pipeline`** (`Code/Pipeline/Physics.Pipeline`) - `PhysicalMaterial` + the collision-
  shape / material asset builders (cook via Jolt).
- **`engine.physics`** (`Code/Engine/Engine.Physics`) - `PhysicsSubsystem` (an `ISceneObserver`; its managers install via the physics `SceneModule` - [[scene-composition]]), the
  components, fixed-step + interpolation, and the `ScenePhysics` script facade.
- **`editor.physics`** (`Code/Editor/Editor.Physics`) - `CollisionShapePage`, the collision-shape
  asset editor. (The collision-group matrix grid is rendered by `Editor.Scene`'s `InspectorView`.)

## Components (value-pool)

- **A moving body (dynamic OR kinematic) needs a CONVEX shape** (box, sphere, capsule, convex
  hull). Jolt's `Shape::MustBeStatic()` names the rest - triangle mesh, plane, heightfield, and
  any compound or decorated shape holding one: no mass, no collision path between two of them,
  and Jolt sets mass properties for every non-static body, so it asserts "Invalid mass" at
  creation (a release build gets a body with no inertia instead). `PhysicsWorld::CreateBody`
  demotes such a body to static with an error, and the scene system names the entity (plane
  and heightfield from the shape kind, a cooked mesh from the collision resource's `convex`
  flag). A convex shape degenerate to zero volume (a hull cooked from a flat quad; Jolt refuses
  a zero scale at shape creation) still collides, so it gets the mass of a solid box over its
  bounds and keeps simulating. Agreed with the Beef port 2026-09-21 (the same fix both sides);
  before it, the editor's Simulate crashed on a dynamic body over a mesh.
- **`RigidBodyComponent`** - motion type (Static/Dynamic + kinematic), mass (0 = computed),
  friction / restitution / damping / gravity factor, sensor, sleep, CCD, DOF locks, and a
  `collisionGroup` (`u8`, `[0, 32)`). Runtime control (`SetLinearVelocity`, `AddForce`/`AddImpulse`,
  `Teleport`, ...) is on the world/body surface.
- **`ColliderComponent`** - ONE collider carrying a `ShapeKind` (Box / Sphere / Capsule / Plane /
  Cooked) with the matching params, plus a `Ref<CollisionShape>` for the `Cooked` case. Multiple
  collider components on a body compound (hierarchy walk stops at nested bodies). Deviation from the
  original plan: one collider component with a kind, not per-primitive `BoxShapeComponent` etc.
- **`CharacterComponent`** - Jolt `CharacterVirtual`. Scriptable directly: reflected `move(velocityX,
  velocityZ)` + `jump(speed)` + `launch(speed)` methods (caller-owned velocity). The standard recipe
  moves a grounded character only by its input, so it stands still on a slope; `drive(x, y, z)`
  hands the whole velocity to the script (gravity included) until the next `move`, and the
  read-only `velocity` (the motion over the last step) and `groundNormal` (up in the air) let a
  board or a sled integrate momentum along the ground.
- **`JointComponent`** - ONE component with a `JointKind` enum (Fixed / Point / Hinge / Slider /
  Distance) referencing a second entity; `motorEnabled` + `motorTargetVelocity` + `motorLimit`
  motors are actually applied. Deviation: one joint component with a kind, not five component types;
  a nil target binds to the nearest ancestor body.

## Scene settings, layers + groups

Per-scene (a `PhysicsSettings` component on the scene, consistent with how gravity lives with the
scene - NOT project settings): `gravity` (`Float3`), the collision-group `groupNames` +
`groupCollides` matrix (up to 32 named groups, NxN mask), and a `debugDraw` toggle. Two-level
filtering: a fixed semantic broadphase layer in the high byte x the designer group in the low byte.
Queries take a group mask, and it is actually applied.

## Stepping + interpolation

A fixed-update lane (`PhysicsSubsystem::OnFixedUpdate(fixedDeltaTime)`) runs 0..N times per frame off
the runtime accumulator. Per fixed step: kinematic bodies take scene transforms via `MoveKinematic`
(velocity-correct), the coalesced world update runs, the contact buffer drains to events, and
dynamic poses are saved as `prev`/`curr`. Every RENDER frame `ApplyInterpolation(alpha)` writes scene
transforms as `lerp(prev, curr, alpha)` + `Slerp` on rotation (scale untouched), so gameplay reads
the smoothed pose. `Teleport` snaps both poses (no ghost lerp). Transform ownership: dynamic = physics
owns pos/rot; kinematic = scene owns; static = immutable while simulating.

## Queries + events

Ray casts, sweeps, and overlaps carry REAL surface normals, body handle, entity, and
distance/fraction. Contact/trigger events are appended to a lock-free buffer on Jolt worker threads
(entity ids packed in body user data - O(1) reverse mapping, no re-entry into Jolt) with an overflow
counter surfaced as a warning, then dispatched on the main thread after the step. Contact data
(relative velocity, combined friction/restitution) comes from Jolt's manifold.

## Resources + cooking

- **`CollisionShape`** (cooked product) - `ConvexHull` or `TriangleMesh` cooked from a source model,
  stored as Jolt `SaveBinaryState` bytes; the factory restores a shared `ShapeRefC` (real Jolt
  refcounting). (Convex decomposition is NOT built - see backlog.)
- **`PhysicalMaterial`** (cooked product) - friction / restitution with combine modes, density, and a
  gameplay `tag`, applied through Jolt's combine callbacks.
- Both resolve through `resource::Ref` and stage into paks; the player needs zero cooking code.

## Editor

`CollisionShapePage` edits the collision-shape asset (cook type + params); the model-import path can
emit a collision shape + rigid body on the generated prefab. The collision-group matrix is a grid
editor in `Editor.Scene`'s `InspectorView` (covered by `Editor.Scene.Tests/CollisionMatrixTests`).
Component gizmos draw the shape wires.

## Script facade

`ScenePhysics.of(scene)` - a reflected, backend-neutral scene-bound value handle:
`rayCast(...)` + `hitX/Y/Z` + `hitNormalX/Y/Z`, `gravityY` / `setGravity(...)`, and
`applyImpulse(entity, x, y, z)`. Physics ops that need world context are keyed on the scene handle
(the component data cannot reach the world); per-character control is on `CharacterComponent`
directly. Certified on Wren (`Engine.Script.Tests/ScriptSceneTests`).

## Portability

Jolt compiles to WASM (single-threaded job fallback via the adapter seam) and ARM/NEON.
Cross-platform determinism is explicitly NOT pursued (revisit only if lockstep networking appears).

## Deferred

Parked, deliberate (convex decomposition / V-HACD, gravity volumes, `JPH_DEBUG_RENDERER` wiring,
per-world job-pool consolidation): `Documentation/Backlog/physics-followups.md`.

---

Design rationale (the reference survey - Sedulous / ezEngine / Godot / Flax - the no-abstraction-
theater call, the layer-model choice, the deviations and their reasons) is in
`Documentation/Archive/physics-design-history.md`.
