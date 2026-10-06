# Vegetation colliders - trees a rider can hit, and find

> STATUS: PROPOSED 2026-10-05 (Snowline P4 needs it for its Forest course). Reviewed by the Sedulous
> session 2026-10-06 (its points folded in: the same scatter and filtering as the drawing, the
> capsule's height foot to top, the query a script uses for an entityless body, readiness, Visible
> and inactive terrain). APPROVED by the user 2026-10-06. BUILT 2026-10-06: the layer's fields came
> in as the component's data version 4 rather than appended fields (an appended field in the middle
> of a layer would misread a binary cook from before; a version-3 layer reads with no collision).

## The problem

Vegetation is scenery only. A procedural layer scatters its instances per chunk (a pure function
of the seed, Foundation::Vegetation) and the vegetation manager draws the chunks near the camera;
nothing about an instance reaches physics. So:

- A rider rides straight through a tree. Meadow keeps its pines 20 m off the course, so it never
  shows; Forest, the course of "lines and shortcuts, dense vegetation, near-misses"
  (Documentation/Specs/snowline.md), needs trees close to the line and solid.
- A script cannot ask where the trees are, so a near-miss (riding past a tree close, fast,
  without touching) cannot be measured.

Placing tree entities by hand where it matters would sidestep both, at the cost of two kinds of
tree (the scattered ones still ghostly) and a generator that duplicates the scatter. The scatter
already knows every tree; physics should get them from it.

## Proposal

### 1. A layer says what of an instance is solid (Engine.Vegetation)

`VegetationLayerBase` gains (the component's data version 4: a layer saved before reads with no
collision):

- `collisionRadius` (f32, metres, 0 = not solid, the default): a trunk's radius;
- `collisionHeight` (f32, metres): the trunk's height above the instance's foot;
- `collisionGroup` (u8, 0..31): the physics collision group of its bodies, so a game can keep them
  apart in its group matrix and find them by group in a query.

All three scale with the instance's scale, read from its matrix (a 1.3x pine has a 1.3x trunk).
Procedural and prop layers alike. `height` is the whole capsule, foot to the top of its cap: the
cylinder between the caps is `height - 2 radius`, at least 0 (a trunk shorter than its width is a
sphere of `radius` sitting on the foot), so both engines build the same body.

`visible` (the layer's view toggle) does not change collision. An effectively inactive terrain
entity gives no capsules, as an inactive rigid body has no body.

### 2. A system can contribute static capsules (Foundation.Scene)

A scene capability beside `IStaticGeometrySource` and `ISceneRayQuery`:

```
struct StaticCapsule { Float3 foot; f32 radius; f32 height; u8 group; };
class IStaticColliderSource
{
    // Appends the capsules this system's static content stands on the world as (world space,
    // upright: from `foot` up `height`, `radius` round). False when its content is not ready yet
    // (nothing appended; asked again later).
    virtual bool CollectStaticCapsules(Scene& scene, Array<StaticCapsule>& out) = 0;
};
// SceneSystem: AsStaticColliderSource() -> IStaticColliderSource*, null by default.
```

The vegetation manager answers: for each solid layer it builds every chunk of the terrain (not
only those near the camera) through the SAME path the drawing builds a chunk with, not a
re-derivation: a procedural layer through the scatter (seed, chunk, heightfield, splat, mask and
its rules: slope, height window, placement), a prop layer through its authored instances as they
are bucketed (an instance on a terrain hole dropped). So the trees the camera draws are the trees
physics has. Instances are terrain-local: a capsule's foot is the terrain's world matrix times the
instance's position.

`CollectStaticCapsules` answers whether its content was ready: a source whose resources (the
heightfield, the splat, the mask, a layer's mesh) are not resolved yet answers false and adds
nothing, and physics asks it again (below).

### 3. Physics builds them (Engine.Physics)

When the physics scene system builds its bodies at scene start (after the rigid bodies, before
the joints and characters), it asks every system for static capsules and creates one static
capsule body each, in the static layer and the capsule's group. A source that was not ready is
asked again each update until it is (its trees become solid the frame its resources resolve; a
source still not ready after a few seconds is logged once). The bodies belong to no entity. They
are dropped with the world when the scene stops.

### 4. A script finds them

Through the queries that report entityless bodies: `ScenePhysics.nearestOverlap(center, radius,
groupMask)` answers a hit whose `entity` is none and whose `position` is the body's ORIGIN (a
capsule's centre: foot + height / 2), and `rayCast` / `sphereCast` with a group mask answer a hit
with no entity at the surface they meet. `overlapSphere` lists entities only, so it never reports a
trunk. Snowline's near-miss is a `nearestOverlap` in the trees' group around the rider that finds a
trunk the rider is not touching.

The game's collision matrix decides what the trees stop: Snowline sets its trees' group to collide
with the rider's explicitly in its scene settings.

## Tests

- Vegetation.Tests / Engine.Vegetation: a solid layer's capsules match its instances (count,
  positions, scaled radius and height); a layer with radius 0 contributes none; the appended
  fields read as zero from a layer saved before them.
- Engine.Physics (or Integration): a scene with a terrain and a solid layer: a character driven
  at a trunk stops; a nearestOverlap in the trees' group at a trunk finds it (no entity, the
  capsule's centre), one beside it does not; a source not ready at start becomes solid once ready.

## Not now

- Shapes other than an upright capsule (a rock's box, a bush's sphere): the capsule covers trees;
  a second shape kind is a later field.
- Streaming colliders with the camera: every solid instance has a body from the start. Snowline's
  forests are a few thousand trees; a world of hundreds of thousands would want the streaming.
- Breakable or moving vegetation.
