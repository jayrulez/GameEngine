# Whiteboxing (blockout geometry for level prototyping)

> STATUS: PROPOSED 2026-09-23, not scheduled; REVIEWED 2026-09-23 (Fable): every citation checked
> against the tree; the collider ruling (compound boxes, built in memory), the load-order note,
> the dedicated whitebox shader, extent clamping and the refuse-existing-mesh rule folded in
> below. Sized M for P0+P1, L with P2+P3. Origin: a user
> question ("what is the best approach for adding whiteboxing to this engine?"), answered after
> reading ezEngine's `ezGreyBoxComponent` and O3DE's WhiteBox gem in full (see "Prior art"),
> then this tree. `paperkid.md` already assumes a blockout primitive kit (its Meshes line names
> `Cube`, `Plane`, `Sphere`, and its scope is "blockout art throughout"), so this spec is the
> thing that game plan is currently doing by hand. Every decision below is language-neutral and
> names the seam it lands on so the Beef port can follow it one for one. Read CONVENTIONS.md
> first.

## Goal

An author drags out a box on the grid, pulls it to height, and has a wall: it draws, it casts
and receives shadows, it collides, it blocks the navmesh, and it can be selected, moved and
resized like anything else in the scene. A room is a dozen of those. When the art arrives, a
blockout piece is replaced in place and the level keeps its layout.

Not goals: boolean CSG (see Decision 2 for why, and what replaces it), UV unwrapping, a modelling
tool with vertex and edge editing, and blockout as a runtime feature - this is authoring
geometry that ships as ordinary meshes or is replaced before ship.

## What exists (checked, cited per the Specs rule)

Grepped before commissioning anything below; all of this is reused rather than rebuilt.

- **Primitive meshes** (`Code/Foundation/Geometry/Primitives.cppm`): `Quad`, `Cube`, `Plane`,
  `Sphere`, `Cylinder`, `Cone`, `Torus`, each returning `RefPtr<StaticMesh>`, plus the private
  `AddFace` / `Finish` helpers (line 315, 333) that every builder writes through. A wedge, a
  stair and an arch are new builders in this file, not a new system.
- **Procedural resources are a supported shape** (`Foundation/Resource/ResourceModule.cppm:127`,
  `Ref<T>`): "Code-created resources (samples, procedural) assign a `RefPtr<T>` directly - the
  direct object wins over the proxy and is never serialized", via `SetDirect`. This is the seam
  a generated blockout mesh binds to, and it is already how `TerrainResource::heightfield`
  carries an in-memory grid with no cook behind it.
- **Mesh rendering** (`Engine.Render/RenderComponents.cppm:47`, `MeshComponent`): holds
  `Ref<StaticMesh> mesh`, a material list, colour, visibility, LOD bias. `InstancedMeshComponent`
  (line 100) holds a mesh plus `Array<Float4x4>` instances and per-instance tints.
- **Collision is already parametric** (`Engine.Physics/PhysicsComponents.cppm:82`,
  `ColliderComponent`): `ShapeKind {Box, Sphere, Capsule, Cooked, Plane, Heightfield}`
  (`Foundation/Physics/PhysicsWorld.cppm:48`) with `halfExtents`, `radius`, `halfHeight`, and
  `Cooked` taking a `Ref<CollisionShape>`, a plain object holding a Jolt shape BLOB
  (`Physics.Resource/PhysicsResource.cppm:49`) that `Ref<T>::SetDirect` can carry with no asset
  behind it. `PhysicsWorld::CookConvexHull(points, outBlob)` (`PhysicsWorld.cppm:279`) builds
  such a blob IN MEMORY. A box blockout needs no new shape at all; every other shape is a
  compound of boxes or a hull built at runtime the same way (Decision 3).
- **The nav bake reads static geometry** (`Editor.Navigation/NavigationBakeImpl.cpp`,
  `CollectNavigationGeometry`): it asks every scene system that answers
  `AsStaticGeometrySource` (physics: static, non-trigger bodies; terrain: its surface) and
  transforms their triangles into zone-local space. Render meshes are not read (since
  2026-10-02: they baked agents into the navmesh). A whitebox piece is in the navmesh through
  its static collision body, which it needs anyway to be solid.
- **Gizmo snapping already exists** (`Editor.Scene/Gizmo.cppm:93-95`): `translateSnap = 1.0f`,
  `rotateSnapDegrees = 15.0f`, `scaleSnap = 0.25f`, with the quantizing documented per drag mode
  at lines 129, 139, 143. Grid-snapped placement is a setting away, not a feature.
- **Modal viewport tools** (`Editor.ViewportTools/ViewportTools.cppm:110`, `IViewportTool`;
  `ViewportToolInput` at line 50; `IViewportToolProvider` at line 215) and their floating panels
  (`Editor.App/ToolPanel.cppm:48`). Terrain sculpt, splat, holes and the two vegetation brushes
  are all built on this; the blockout tool is another one.
- **Per-component viewport gizmos** (`Editor.Scene/ComponentGizmos.cppm`): `IGizmoRenderer` +
  registry, type-erased on reflection instances, drawn through debug-draw. This is where face
  handles hang.
- **NO triplanar or world-projected surface shading exists.** The only box projection in
  `Data/Engine/Shaders` is the reflection-probe parallax correction (`forward.ps.hlsl:446-449`), which
  is a cubemap ray, not a surface UV. Decision 4 commissions this, and it is the one genuinely
  new piece of rendering work in the spec.

## Prior art (both read in full, 2026-09-23)

The two engines sit at opposite ends of the design space, which makes the comparison unusually
clean.

**ezEngine - `ezGreyBoxComponent`** (`Code/Engine/GameEngine/Gameplay/GreyBoxComponent.h`,
one component, no new asset type, no mesh editing):

- Shapes: `Box`, `RampPosX/NegX/PosY/NegY`, `Column`, `StairsPosX/NegX/PosY/NegY`, `ArchX`,
  `ArchY`, `SpiralStairs`. Direction is baked into the enumerator rather than expressed as a
  rotation.
- SIX independent extents (`SizeNegX`, `SizePosX`, `SizeNegY`, ...), not a centred half-extent.
  The origin stays put while faces move independently. Extents crossing zero flip the winding
  (`bInvertedGeo` in `GenerateMeshResourceDescriptor`).
- Dedup by generated name: `GenerateMeshName` formats every geometry-affecting parameter into a
  string, then `GetExistingResource` before `GetOrCreateResource`. Identical blocks share one
  mesh with no instancing machinery.
- Declarative face handles: `ezNonUniformBoxManipulatorAttribute("SizeNegX", "SizePosX", ...)`
  on the component's reflection block - six drag handles derived from the property names.
- Placement: `ezDrawBoxGizmo` (382 lines) and, on `EndInteractions`, one undo transaction
  creating the entity, setting its position, adding the component, and adopting whatever
  material is selected in the asset browser.
- Extra outputs: `GenerateCollision`, `UseAsOccluder` (blockout feeds the software occlusion
  rasterizer), and message handlers for `ezMsgBuildStaticMesh` (level export bake) and
  `ezMsgExtractGeometry` (navmesh).
- Material: defaults to a `Pattern` asset whose shader calls
  `SampleTexture3Way(BaseTexture, sampler, Normal, WorldPosition, 0.25)` -- triplanar from world
  position at a fixed tiling (`Data/Base/Shaders/Materials/MaterialHelper.h:271`).

**O3DE - the WhiteBox gem** (`Gems/WhiteBox`, 22,161 lines in the gem alone, plus OpenMesh for
half-edge topology and Manifold v3.5.1 for booleans, both fetched as external dependencies):

- A real editable mesh: vertex, edge and polygon translation and scale modifiers, an edge-restore
  mode, a transform mode, a draw-shape mode. `WhiteBoxToolApi` is 820 + 3,575 lines by itself.
- Shapes: `Cube`, `Tetrahedron`, `Icosahedron`, `Cylinder`, `Sphere`, `Asset`; the draw tool
  builds Box / Cylinder / Pyramid / Cone / Sphere / Staircase from a drawn footprint plus a pull
  height.
- The CSG cost is visible in the code: `GroupCoplanarTriangles` (regrouping boolean output back
  into polygons so interior edges hide), `EarClip`, vertex welding, double-precision 2D
  predicates (`Core/WhiteBoxCsg.h`).
- UVs are baked per face on the CPU (`Util/WhiteBoxTextureUtil.cpp:36`,
  `CreatePlanarUVFromVertex`): pick the dominant normal axis, swizzle world position. It
  truncates the normal to three decimals first, because grid-snap noise made faces flip between
  projection planes.
- Storage: the mesh serializes inline in the component (`m_whiteBoxData`), with optional
  promotion to a shared `.wbm` asset. Inline until shared.
- Handoff: `ExportToFile` writes `.obj`, and the normal asset pipeline takes over.

**What they agree on**, which is the load-bearing part:

1. Neither uses mesh UVs. Both project from world position at a fixed density - ez in the
   shader, O3DE baked per face. This is why Decision 4 is P0 and not polish.
2. The same core gesture, arrived at independently: draw a footprint rectangle, pull for height.
3. Blockout data lives in the level, not as one asset per block.
4. Stairs are special-cased in both. Not something to fake with a ramp.

They diverge only on booleans: ez has none and covers holes with arch primitives; O3DE bolted on
Manifold rather than write its own.

## Decision 1 - a blockout piece is PARAMETRIC data, and the mesh is derived

`WhiteboxComponent` stores what the piece IS - a shape enumerator, six extents, and the
shape-specific parameters - never vertices. The mesh is a pure function of those values,
regenerated whenever they change and shared between identical pieces.

Six independent extents (`sizeNegX`, `sizePosX`, `sizeNegY`, `sizePosY`, `sizeNegZ`, `sizePosZ`),
following ez. A centred half-extent forces the origin to move whenever one face does, which makes
face-dragging fight the transform gizmo; independent extents let a face move while the entity's
transform sits still. Each extent is CLAMPED at a small positive minimum (`kMinExtent`, 0.01 m):
the origin always sits inside the piece, and ez's inverted-solid branch (an extent dragged past
zero flipping the winding, `bInvertedGeo`) is not ported - it buys nothing and it is the easiest
thing for a face handle to do by accident.

Rejected: storing generated vertices in the component. It bloats the scene file with data that is
fully derived, and it makes a shape parameter edit a mesh rewrite rather than a cache miss.

## Decision 2 - no boolean CSG; a doorway is a SHAPE

The classic blockout workflow is subtractive brushes, and this spec deliberately does not build
them. O3DE's gem is the measurement: 22k lines plus two external libraries, and the boolean is
the easy half - regrouping coplanar triangles back into polygons, ear-clipping, welding and
float-robust 2D predicates are the rest. ez ships no booleans at all and has not visibly
suffered for it.

What people actually cut is doorways and windows, so those become primitive shapes: a box with a
rectangular hole through one axis, parameterised by the opening's size and offset. That plus
ramps, stairs and arches covers the overwhelming majority of subtractive intent at a fraction of
the cost, and it keeps every piece a closed convex-decomposable solid, which the collision path
wants anyway.

If booleans are ever wanted, they arrive as a BAKE (select pieces, produce one static mesh
asset), not as live geometry - and Manifold is the library to reach for rather than a hand-rolled
clipper.

## Decision 3 - the component DRIVES a MeshComponent and a ColliderComponent

This is the one place the spec departs from both prior engines, and it is because of what this
tree already does.

ez makes `ezGreyBoxComponent` a `ezRenderComponent` in its own right, then re-exposes collision,
occlusion, navmesh geometry and static-mesh baking through four separate message handlers. Here,
`NavigationBakeImpl.cpp:114` walks `MeshComponentManager` and `TerrainComponentManager` - a
self-rendering blockout component is invisible to the navmesh until a third collector is added,
and the same argument repeats for every future consumer of "the static geometry in this scene".

So: `WhiteboxComponent` is AUTHORING data that writes into the sibling `MeshComponent` (via
`Ref<StaticMesh>::SetDirect` with the generated mesh) and `ColliderComponent` on the same entity,
and owns nothing about drawing. Rendering, shadows, GPU picking, LOD, material assignment, the
nav bake, and anything added later all work with no integration at all.

**The collider is a compound of boxes, never a hull of the whole piece** (review 2026-09-23). A
convex hull of a doorway fills the door, of a window fills the window, of an arch fills the arch:
the nav bake and the player both find a solid wall. Decision 2 keeps every piece
convex-decomposable, so the generator emits the decomposition alongside the triangles: a `Box`
shape for a box or a column; a Jolt static-compound blob of boxes for a doorway (two jambs, a
lintel, the wall above), a window (four boxes), an arch (boxes stepping the curve at `detail`)
and a stair (one box per step); a convex hull only for a wedge / ramp, built with
`CookConvexHull`. A stair's collision is its STEPS, not a ramp (a capsule walks steps; a ramp
would be the cheaper choice and the wrong shape for a placed prop). Every blob is built IN
MEMORY on the parameter change and handed to the collider through `Ref<CollisionShape>::SetDirect`
- nothing cooks through the pipeline - and it is cached beside the mesh under the same
parameter hash (Decision 5), or a face drag re-cooks a Jolt shape at every step.

**On scene load the mesh is regenerated, not read.** A direct object on a `Ref<T>` is never
serialized (`ResourceModule.cppm:127`), so a loaded `MeshComponent` sibling holds a nil mesh
until the whitebox manager runs. The manager regenerates every piece on scene start and syncs
on a parameter change, and its `UpdateOrder` places it BEFORE `MeshComponentManager`'s
extraction so no frame draws nothing. The sibling's `mesh` row is READ-ONLY in the inspector
(an empty Mesh field beside a working blockout reads as a bug); the material is chosen on the
whitebox component and written through to the sibling's material list.

**An entity that already has a `MeshComponent` REFUSES a whitebox component**: the component
menu greys the entry (the displayName + category attributes carry a `requires` / `excludes`
hint, or the add path returns a status the menu shows). Adopting the existing mesh silently
would overwrite authored data. Destroying the whitebox component leaves the siblings in place
with their last mesh and collider - a deliberate "bake in place" - and the status bar says so.

The cost is that two components must stay in step, which the manager does in one place on a
parameter change, and that a blockout entity carries three components rather than one. That is a
smaller cost than a parallel geometry path through every consumer.

Rejected: a self-rendering component (ez's shape). Rejected: an `InstancedMeshComponent` batching
path per shape kind - blockout pieces need individual selection and per-piece materials, and
Decision 5's mesh sharing already removes the memory argument.

## Decision 4 - blockout reads at a glance, which means world-projected UVs

A blockout whose grid texture stretches when a block is resized is unreadable: judging scale and
proportion at a glance is the entire point of the exercise, and stretched texels destroy it. Both
prior engines solve this and neither uses the mesh's own UVs.

`Data/Engine/Shaders` has no triplanar path today (checked: the only box projection is the
reflection-probe parallax at `forward.ps.hlsl:446`), so this commissions one: a DEDICATED
`whitebox` material shader (`whitebox.ps.hlsl` over the shared forward vertex path and the
forward includes; a material names it with `MaterialBuilder(...).Shader(u8"whitebox")`, the
model the renderer already supports for custom materials without renderer changes), sampling
the base texture from WORLD position on the three axis planes and blending by the normal, at a
fixed tiling (ez uses 0.25, i.e. a four-metre repeat). NOT a `WHITEBOX` flag on the forward
pixel shader: a flag bit is a global resource that multiplies the forward's cooked lattice for a
feature only blockout uses. The depth, shadow and pick passes need nothing - the piece is an
ordinary mesh - so only the colour pixel shader is new, and it cooks to WGSL like any other
(the probe runs on Vulkan + WebGPU). Plus a shipped grid/checker texture and a default whitebox
material.

Shader-side projection is chosen over O3DE's CPU-baked planar UVs so that nothing has to be
recomputed when a piece moves or resizes. It also sidesteps O3DE's grid-snap bug outright: they
had to truncate the normal to three decimals because snap noise flipped faces between projection
planes between edits. A shader that re-derives the blend every frame from the interpolated normal
has no stored choice to flip, but the blend weights should still use a wide exponent rather than
a hard pick, for the same underlying reason.

## Decision 5 - identical pieces share one mesh, keyed by their parameters

Following ez exactly: a cache keyed by a hash (or formatted name) over every geometry-affecting
parameter - shape, the six extents, detail, and the shape-specific values. A miss generates; a
hit returns the existing `RefPtr<StaticMesh>`. A room of identical 4x4x1 wall panels holds one
mesh.

This is why no instancing path is needed: the renderer keys its GPU buffers on the mesh's
identity, so identical pieces already share one upload with nothing added. It also makes the
parameter-change path cheap: editing one piece's height is a cache lookup, and dragging a face
through values that have been seen before costs nothing. The cache entry holds the mesh AND the
collision blob (Decision 3).

The cache is owned by the component manager, keyed by the parameter hash, and entries are dropped
when their last referencing component goes - the same shape as
`TerrainHoledMeshCache` / `TerrainHoleTextureCache` in `Engine.Terrain`.

## Data model (the whole delta)

```
foundation.geometry     Primitives + Wedge, Stairs, Arch, Doorway, Window builders
                        (through the existing AddFace/Finish helpers)

engine.whitebox         WhiteboxShape enum {Box, RampX/Y(+/-), Column, StairsX/Y(+/-),
  (new module)            ArchX/Y, Doorway, Window}
                        WhiteboxComponent {shape, 6 extents, detail, thickness,
                          openingSize/openingOffset (Doorway/Window), curvature,
                          generateCollision, material}
                        WhiteboxComponentManager: the parameter-hash cache (mesh + the
                          collision blob); writes the sibling MeshComponent.mesh and
                          ColliderComponent on a parameter change and on scene start;
                          UpdateOrder before MeshComponentManager

Data/Engine/Shaders            + whitebox.ps.hlsl (a dedicated material shader over the shared
                          forward vertex path): world-projected triplanar sampling at
                          fixed tiling
Data/Materials          + Whitebox.ezMaterialAsset equivalent + a grid texture

editor.whitebox         WhiteboxTool ("whitebox.place"): drag a footprint on the grid
  (new module)            or a surface, pull for height, one command per gesture
                        WhiteboxPanelProvider: shape picker, size readout, detail
                        Face handles through ComponentGizmos (six, from the six extents)
                        "Replace with asset" acting on a selection
```

Nothing changes in `MeshComponent`, `ColliderComponent`, the nav bake, the renderer or the
gizmo - which is Decision 3 paying for itself.

## Phases

- **P0 - the piece** (Foundation + Engine, no editor): the shape enumerator, the component, the
  parameter-hash mesh cache, the sibling-component writes, the new primitive builders, and the
  whitebox material with world-projected UVs. Acceptance: a scene with a hand-authored
  `WhiteboxComponent` draws a correctly proportioned box with an unstretched grid at any size,
  collides, and appears in a nav bake; two identical pieces share one mesh (the cache reports
  one entry); a pixel probe on Vulkan and WebGPU shows the grid texel density unchanged when the
  extents double.
- **P1 - the tool**: `whitebox.place`, drag-a-footprint-then-pull, grid snapping from the
  existing gizmo settings, six face handles on the selected piece, one `EditorCommandStack`
  command per gesture, and the panel. Acceptance: a headless gesture through `ViewportToolInput`
  creates a piece of the dragged size, a face drag resizes exactly one extent, and both undo as
  one step.
- **P2 - the kit**: ramps, stairs, columns, arches, doorways and windows; per-piece tint as a
  tagging device (walkable / blocking / prop placeholder). Acceptance: each shape's collision
  matches its visual within a tolerance the test states; a doorway's opening is passable in the
  nav bake.
- **P3 - handoff**: "replace selection with asset" keeping transforms, and an optional bake of a
  selection to one static mesh asset - a `StaticMeshAsset` with its `.geometry.bin` sidecar
  (the bulk-data sidecar rule; the shape paperkid.md's mesh line already names). Acceptance:
  replacing a blockout piece with a mesh asset leaves the transform untouched; a baked selection
  round-trips through the asset pipeline.

## Gotchas

- **Extents never cross zero.** The clamp in Decision 1 is the whole defence; a face handle
  that lets an extent reach zero has a bug in the handle, not a case for the generator.
- **A hull is the wrong collider for a hole.** Doorway, window and arch collide as compounds of
  boxes (Decision 3); a test with a capsule walking through a doorway pins it.
- **A loaded scene has no mesh until the manager runs.** Regenerate on scene start, before the
  mesh manager extracts (UpdateOrder); the test loads a saved scene and extracts on the first
  frame.
- **Grid-snap noise and projection choice.** O3DE truncates normals to three decimals before
  picking a projection plane because snapping perturbed the normal enough to flip the choice
  between edits. Decision 4 avoids the stored-choice version of this bug, but any CPU-side
  planar decision added later inherits it.
- **The collider must follow the mesh, not lag it.** A parameter change writes both, in the same
  place, in the same frame. A blockout whose collision is one edit behind is worse than no
  collision, because it looks right.
- **Two components to keep in step (Decision 3's cost).** Destroying the whitebox component
  should not silently leave a mesh and collider behind, and adding one to an entity that already
  has a `MeshComponent` must be refused or must adopt it deliberately - pick one and state it in
  the implementation, because the silent-overwrite version loses authored data.
- **Cache eviction.** Keyed by parameter hash, so dragging a face sweeps through many keys in one
  gesture. Evict on last-reference-dropped rather than growing a cache with every intermediate
  size a drag passed through.
- **`Detail` changes geometry, so it is part of the cache key** - as are curvature and thickness
  for the curved shapes. ez's `GenerateMeshName` includes exactly these and it is worth copying
  the list rather than rediscovering it.

## Tests (the spec's contract)

Foundation: each new primitive builder produces a closed manifold with outward normals, and its
AABB matches the requested extents; an extent below the minimum clamps. Engine: the
parameter-hash cache returns one mesh for two identical components and two for differing ones,
and drops an entry when its last holder goes; a parameter change rewrites both the sibling mesh
reference and the collider; a doorway's compound collider lets a capsule through the opening and
stops it at the jambs (Physics.Tests); a saved scene reloads and draws its pieces on the first
extraction; adding a whitebox component to an entity with a mesh component is refused; a whitebox
piece appears in `CollectNavigationGeometry`. Rendering:
a pixel probe (Vulkan + WebGPU) showing the projected grid holds its texel density across a
resize, and reads the same on both backends. Editor: a scripted place gesture creates a piece of
the dragged size and undoes as one command; a face-handle drag changes one extent only; the
status names the shape and size.
