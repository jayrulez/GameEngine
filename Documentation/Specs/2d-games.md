# 2D games - sprites, an orthographic camera, sort order, locked-axis physics, tilemaps

**Status:** PROPOSED 2026-09-26 (inventory + design; nothing built). Asked for by the user on
2026-09-26 ("what would it take for us to support 2D games?") while the Beef side caught up.
The layers are ordered so that a 2D game is possible after the first four, before any
tilemap or editor tooling exists.

## The problem as it stands

The engine is a 3D clustered forward+ engine. A 2D game today would run on one usable
building block and hit walls everywhere else. The inventory (2026-09-26):

- **Sprites exist.** `SpriteComponent` (`Code/Engine/Engine.Render/RenderComponents.cppm`):
  texture, `size` in world units, `uvRect` (an atlas sub-rect), tint, orientation
  (CameraFacing / CameraFacingY / WorldAligned / EntityOriented), additive, postTonemap,
  visible. Extracted in `ExtractImpl.cpp`, drawn instanced and batched by the
  `SpriteRenderer` (`Code/Foundation/Render/SpriteRenderer.cppm`), unlit
  (`Data/Engine/Shaders/sprite.ps.hlsl`), alpha or additive, depth-tested, no depth write. The
  script facade `SpriteComponent.of(e)` reaches texture, size, uvRect, tint, orientation,
  additive, visible.
- **Sprite bugs and gaps.** The sampler is hard-coded Linear (`SpriteRendererImpl.cpp:69`),
  so a texture imported with the sprite preset (Nearest, no mips, sRGB -
  `TextureAsset::SetupForSprite`) still blurs. `EntityOriented` and `postTonemap` are not
  reflected. Culling uses `size` and ignores the entity's scale. No flip, no pivot, no
  per-sprite rotation for a camera-facing sprite, no sort order.
- **No orthographic camera.** `CameraComponent` has fovY, aspect, near, far, clearColor,
  primary; the projection is always `PerspectiveFovRH` (`ExtractImpl.cpp:398`).
  `Float4x4::OrthographicRH` exists (reverse-Z) and shadows and the thumbnail stage use it;
  LOD selection and culling already handle an orthographic view.
- **No defined order between coplanar sprites.** The sort key is category + state + depth
  (`Code/Foundation/Render/Views.cppm:133`). Under an orthographic camera two sprites at
  the same depth draw in an undefined order, and a top-down game needs Y sorting.
- **Sprites go through the HDR post stack.** The Transparent category gets exposure, bloom
  and the tonemap unless `postTonemap` is set. There is no unlit 2D-only path, and no
  reason to build one: `postTonemap` is the switch.
- **Physics is Jolt, 3D.** `RigidBody`, `Collider`, `Character`, `Joint`
  (`Code/Engine/Engine.Physics/PhysicsComponents.cppm`); shapes Box, Sphere, Capsule,
  Plane, Cooked, Heightfield. Jolt's allowed degrees of freedom are not exposed, so a body
  cannot be kept in a plane.
- **No tilemap, no tileset, no Tiled or LDtk importer.** Nothing in code or docs.
- **Atlas and 9-slice exist for the UI only.** `ImageAtlasBuilder` (shelf packer,
  `Code/Foundation/Image/AtlasBuilder.cppm`) packs UI theme atlases; `NineSlice` and the
  VG drawables serve the UI. The particle system has a flipbook (`FlipbookSettings`).
  `PropertyAnimation` tracks are Float, Float3, Color and Quat: no step track, so `uvRect`
  cannot be keyframed.
- **Navigation is Recast.** No grid pathfinding. Audio has a `spatial` bool (a 2D game
  turns it off). Input's composite 2D axes serve a 2D game as they are.
- **The docs never planned or deferred 2D.** Sprites appear only as a renderer feature, as
  the particle and world-UI substrate, and as a texture preset.

## Prior art (read 2026-09-26; ~/Dev/CPP/ZeroCore, ~/Dev/CPP/ezEngine; Unity and Godot from memory)

**Zero (ZeroCore)** is the closest to what we want, because it is a 3D engine that ships 2D
games as a first-class case INSIDE the 3D world:

- The camera has a `PerspectiveMode` (perspective or orthographic) on the one component
  (`Code/Systems/Graphics/Camera.hpp:40`). No second camera type.
- Physics has `Mode2D` on the space and on each body (`PhysicsSpace.hpp:10`,
  `RigidBody.hpp:17`, with `Inherit2DMode` so a body follows the space by default): the
  body's movement is restricted to the plane and its rotation to the plane's normal. One
  physics engine, one world, one set of colliders.
- A `SpriteSource` resource owns the frames: `FrameCount`, `FrameRate`, `Looping`,
  `PixelsPerUnit`, an `Origin` (`SpriteSource.hpp`, `Content/SpriteBuilder.hpp`), packed
  into an atlas at build time (`Graphics/Atlas.hpp`). The sprite component references the
  source; a sprite's world size is pixels over PixelsPerUnit, so art authored at a pixel
  scale lands at a consistent world scale.
- A `TileMapSource` resource (`Extensions/Gameplay/TileMapSource.hpp`) holds the tiles; the
  tile map component draws them and builds collision from them.

**ezEngine** has an orthographic camera mode on its camera (`ezCameraMode::OrthoFixedWidth`
/ `OrthoFixedHeight`, `Code/Engine/Core/Graphics/Camera.h`), which decides the useful
question for an ortho camera: which axis is fixed when the aspect changes. Its sprite
component is a billboard for markers, not a 2D game sprite. No tilemap, no 2D physics.

**Unity** (from memory) is Zero's approach at scale: 2D lives in the 3D world with an
orthographic camera, a SpriteRenderer with sorting layers and order-in-layer, a Rigidbody2D
on a separate 2D physics engine (Box2D), a Tilemap package. The separate physics engine is
the part that costs Unity the most (two physics worlds, two collider families, no mixing).

**Godot** (from memory) keeps a separate 2D scene tree with its own nodes, own physics and
own renderer. Clean for a pure 2D game, but every system exists twice and a 2.5D game
(3D world, 2D gameplay) falls between the two.

**Lumix** and **Traktor** have no 2D game support (Traktor's Spark is a Flash player).

**Conclusion.** Zero's shape, with ezEngine's ortho-axis rule: 2D inside the 3D world, one
camera component with a projection mode, one physics engine with a plane lock, sprites
with a sort order and a sprite-sheet resource, a tilemap resource on top. Nothing exists
twice. A 2.5D game (a 3D world seen through an orthographic camera, 2D gameplay) costs
nothing extra, which is what a small team actually ends up shipping.

## What already fits

- The reference shape (`ReferenceTraits`, `ReferenceOps`) means a sprite sheet asset
  reference is inspectable and writable through `entity_inspect` / `component_set` for
  free, and the MCP loop drives the 2D sample as it drove PaperKid.
- `SpriteRenderer` batching by (texture, blend) is exactly what an atlas needs: one sheet,
  one draw for every sprite on it.
- `ImageAtlasBuilder` packs; the cook can use it for sprite sheets as the theme cook does.
- The particle flipbook and `uvRect` prove the frame-by-rect model; the sheet resource
  formalises it.
- `OrthographicRH`, the LOD and culling math, and the thumbnail stage's orthographic view.
- Jolt exposes `EAllowedDOFs` per body; the lock is a body-creation setting, not a new
  engine.
- The action system, the page system and the MCP bridge: the 2D editor tools (layer 6)
  are actions, pages and tools like every other.

## Design

### Layer 0 - sprite fixes (days)

- The sprite sampler follows the texture resource's sampler (the sprite preset's Nearest
  and clamp), not a hard-coded Linear. One sampler per (texture) in the batch key.
- `EntityOriented` and `postTonemap` reflected; `flipX` / `flipY` and a `pivot` (0..1 in
  the sprite's rect, default centre) added and reflected; culling takes the entity scale.
- A `sortOrder` (i32) on the component, reflected, carried by the extract. Its use is
  layer 1's.

### Layer 1 - the orthographic camera and a defined order (days)

- `CameraComponent` grows `projection` (Perspective | Orthographic), `orthoSize` (the
  fixed half-extent) and `orthoFixedAxis` (Height | Width - ezEngine's rule for what the
  aspect changes). `ExtractImpl` builds `OrthographicRH` for the mode; the editor camera
  preview and the game host follow. Scripts read and write them through the existing
  facade.
- The sort key gains a layer and order: `sortLayer` (u8, from a per-project layer list in
  the project settings, not a hard-coded enum) and `sortOrder` (i32) BEFORE depth for the
  sprite and transparent categories, then depth, with a per-layer `sortAxis` (Depth | Y)
  so a top-down layer sorts by Y instead. Coplanar sprites are ordered; a 3D transparent
  mesh keeps sorting by depth on the default layer.
- `pixelsPerUnit` on the sprite sheet (layer 2) and on a bare sprite texture: the sprite's
  world size defaults to pixels over units, so the camera's `orthoSize` and the art agree.

### Layer 2 - sprite sheets and animation (about a week)

- A `SpriteSheet` asset (Pipeline): frames from a grid (columns, rows, count, padding) or
  from packing a set of images through `ImageAtlasBuilder` at cook; `pixelsPerUnit`, a
  default pivot, named clips {name, first, count, fps, looping, events by frame}. Cooked
  product: the atlas texture reference plus the frame rects and clips.
- A `SpriteAnimatorComponent` (Engine): sheet reference, current clip, time, speed, playing;
  drives the sibling `SpriteComponent`'s texture and `uvRect` each tick; clip events reach
  scripts the way animation-graph events do. Script facade `SpriteAnimator.of(e)`: `play`,
  `stop`, `clip`, `frame`.
- The particle flipbook stays as it is; a later step can point it at a sheet.

### Layer 3 - 2D physics on Jolt (days)

- `RigidBody` grows `planeLock` (None | XY | XZ): Jolt `EAllowedDOFs` translation in the
  plane and rotation about its normal. A scene physics setting supplies the default (Zero's
  space-level `Mode2D` with `Inherit`), so a 2D scene sets it once.
- Thin colliders are the existing Box and Capsule with a depth; nothing new. One-way
  platforms through the contact listener (ignore a contact whose normal opposes the
  platform's up).
- `Character` stays: it already walks a capsule against Jolt shapes; with the lock it is
  the platformer controller.
- Units: metres, as the rest of the engine. `pixelsPerUnit` is the only conversion, and it
  lives in the art, not the physics.

### Layer 4 - a 2D sample (with the layers above; the driver)

- A small platformer room or a top-down screen under `Data/SampleProjects/`, built through
  the MCP loop the way PaperKid was: `scene_write`, `entity_inspect`, `component_set`,
  `viewport_camera_set` and `viewport_screenshot` for every visual check. It is the
  acceptance test for layers 0 to 3 and the thing the user looks at.

### Layer 5 - tilemaps (one to two weeks)

- A `Tileset` asset: a sprite sheet plus per-tile data (collision shape: none, full,
  a convex polygon; a tag; an animation clip for animated tiles).
- A `TilemapComponent`: chunked layers of tile indices (a chunk is one instanced sprite
  draw), a `cellSize`, per-layer sort layer and order, collision merged per chunk into a
  cooked convex compound (rectangles merged greedily, then the polygons) on a static body.
- A Tiled (.tmx/.tsx) importer first, LDtk second; both are XML/JSON over the same
  tileset and layer model, and both are what 2D artists already use.

### Layer 6 - editor tooling (weeks; tile painting is the bulk)

- A 2D viewport mode on the scene page: the editor camera orthographic, grid snapping in
  cell units, sprite handles (pivot, flip) on the select tool, a sort-order readout in the
  inspector. All as actions over the page, so they reach the palette and the MCP bridge.
- A sprite sheet page: slicing preview, clip list, a play button.
- A tilemap tool: a viewport tool (the same `ViewportToolHostContext` family as the
  terrain brushes) painting tile indices with a palette panel, fill, erase, and a
  collision overlay.

### Layer 7 - grid pathfinding (days)

- An A* over the tilemap's collision layer as a second `NavAgent` provider: the agent
  interface stays, the world is a grid.

### Not built yet, on purpose

- 2D lighting (normal-mapped sprites, 2D shadows): the forward+ renderer already lights the
  world; sprites are unlit by choice until a game asks.
- A separate 2D render path: `postTonemap` and the sort layer cover it.
- Box2D: only if a shipped platformer's feel demands it, judged with the sample in hand,
  never before.
- An in-editor animation timeline for sprites: clips are data in the sheet; the page plays
  them.

## The two decisions

1. **Jolt with a plane lock, not Box2D.** One physics world, `Character` keeps working, no
   new dependency on either side (the Beef side binds Jolt already). Box2D earns its place
   only if the sample's feel demands it.
2. **Sort layer and order in the sort key, not a Z trick.** Giving each layer a Z offset
   works under an orthographic camera and costs nothing, but coplanar order stays undefined
   and top-down games need Y sorting. The key change touches one place and is principled.

## Where this touches the Beef side

Everything ports as usual. Layers 0 to 3 are Engine.Render, Engine.Physics and Pipeline:
new engine code with few editor ties, which the Beef agent can port in parallel commit for
commit. Layer 6 lands last on both sides.

## Landing order (each its own commit with tests, both compilers, then the lanes)

0. Sprite fixes (sampler, reflection, flip, pivot, scale-aware culling, sortOrder).
1. Camera projection mode + ortho size + fixed axis; sort layer, order and axis in the key;
   pixelsPerUnit. Tests: extract builds the ortho projection; the sort key orders coplanar
   sprites and Y-sorts a top-down layer.
2. SpriteSheet asset + cook; SpriteAnimatorComponent + facade. Tests: grid slicing,
   packing, a clip advancing frames and firing an event.
3. planeLock on RigidBody + the scene default; one-way platforms. Tests: a locked body
   never leaves the plane; a body from below passes a one-way platform, from above lands.
4. The 2D sample, through the MCP loop; its scene stream in SampleProjectTests.
5. Tileset + Tilemap + merged collision + the Tiled importer. Tests: chunk draw counts, the
   merged shapes, a .tmx round trip.
6. The 2D viewport mode, the sheet page, the tilemap tool.
7. Grid A* behind NavAgent.

Layers 0 to 4 make a 2D game possible; 5 to 7 make it comfortable.
