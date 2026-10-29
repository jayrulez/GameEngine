# Wind for every shader - the sway read from the draw, not from PBR's material layout

> Status: APPROVED 2026-10-29, not built (user: "Let's look into this properly.
> Explain how it is built into PBR, and why it can't be used in custom shader"; "Wind write up").
> Phase 2 of `shader-blocks-cleanup.md`, which removes every borrowed lane, the clock's included.
> Read CONVENTIONS.md first.

## Why

Wind, the vertex sway for grass, leaves and cards, works only for a material on the built-in PBR
shader. A custom shader cannot take part in it, and one that names its properties as PBR does can
make its shadow sway while the mesh stands still. The cause is one design decision: the shared
vertex stages read the wind from the material's buffer in PBR's layout. Every other material
layout (Phong, Unlit, every custom shader) is wrong for them.

No sample uses wind today (no material in any sample project has a WindStrength above zero; the
vegetation scatter makes no wind materials of its own), so this can change without moving any
content. The tests that cover it: `Render.Tests/WindTests.cpp`, `Render.Backend.Tests/
WindProbeTests.cpp` (a card's tips sway, its roots stay, a still material byte-identical, on Vulkan
and WebGPU), `Materials.Tests`, and `Integration.Shaders/SurfaceEquivalenceProbeTests.cpp`.

## How it is built today

Four pieces; the first two assume PBR's layout.

1. **The material.** `CreatePBR` (`Foundation/Materials/MaterialBuilder.cppm`) puts `WindStrength`,
   `WindSpeed` and `WindHeight` in the spare lanes of PBR's 64-byte material buffer, at byte
   offsets 24, 28 and 60. `MaterialSystem` makes that buffer visible to the vertex stage as well as
   the pixel stage, for wind.
2. **The shaders.** `forward_vs.hlsli` (PBR's, Phong's and Unlit's vertex stage, and a low-level
   custom shader's when it includes it), `shadow_depth_vs.hlsli` and `pick_ids_vs.hlsli` include
   `wind.hlsli` under `#ifdef WIND`. `wind.hlsli` DECLARES the material buffer itself,
   `cbuffer Material : register(b0, space2)` with PBR's fields in PBR's order, and `WindSway()`
   reads the three wind fields from it. The maths alone is `wind_sway.hlsli`'s
   `WindSwayOf(worldPos, localY, time, strength, speed, height)`.
3. **The renderer.** `MeshRenderer::MaterialWantsWind`: a material whose float `WindStrength` has a
   default above zero. Then the forward pipeline, the shadow pipeline (`ShadowConfigFor`) and the
   pick pipeline (`PickConfigFor`) get the `WIND` flag, and the shadow and pick draws bind the
   material's buffer (`DepthLikeFor`'s `materialSet`).
4. **The clock.** The scene's clock (scaled, pausable) reaches each pass its own way: the forward
   pass in `IblParams.zw` (this frame's and last frame's, for motion vectors), the shadow pass in
   `ShadowWind.x`, the pick pass in `WindTime`.

## Why a custom shader cannot use it

- **The buffer clashes.** A custom shader's `properties` block generates its own `Material` buffer
  at the same register, in its own layout. A low-level shader that lists `WIND` in its vertex
  variants and includes `forward_vs.hlsli` pulls in `wind.hlsli`'s second declaration of it. Were
  the names not to clash, offsets 24, 28 and 60 are still PBR's.
- **Without the flag, nothing sways.** A request for a flag a shader does not declare is dropped
  (`Foundation/Shaders/Variants.cppm`: every request is intersected with the declared mask), so
  the visible mesh of a custom material never sways.
- **The shadow and the pick can sway wrongly** (read from the code, not run). The built-in shadow
  and pick programs serve every material, so they can only know one layout. A custom material
  with no depth or pick program of its own (a low-level shader has none; `ShaderResource::
  DepthProgram`/`PickProgram` are a surface shader's) that declares a float `WindStrength` above
  zero is drawn into the shadow map and the pick buffer by them, with `WIND`, reading its buffer
  at PBR's offsets. `material_set_shader` keeps values whose name and type fit, so a windy PBR
  material moved to such a shader would keep its strength and trigger this.
- **Phong and Unlit carry dead variants.** Both vertex stages list `WIND`; neither material has a
  `WindStrength`, so it is never chosen, and if it were, it would read PBR's offsets from their
  buffers.
- **What works now** is a copy, not the feature: a surface shader can call `WindSwayOf` from its
  `Vertex()` with properties of its own (the surface PBR in `Integration.Shaders/
  SurfaceEquivalenceProbeTests.cpp` does, in PBR's layout); its generated depth and pick programs
  follow because they are built from the same `Vertex()`.

## The design: wind is an effect with a block of its own

The vertex stages read wind from a block of its own, bound only for a draw whose pipeline has
`WIND`, which the renderer fills from the material's Wind parameters. The material's layout no
longer matters, and the built-in shadow and pick programs, which serve every material, read the
same block the forward stage does.

Ruled 2026-10-29 (user: "just slotting unrelated things in unused blocks"): wind does NOT go into
spare or padding lanes of an existing block; that is how it came to be tied to PBR. Two named ways
were weighed:

- **A. Named fields in each pass's per-draw block** (`Object` grows a row). Simplest; every draw
  carries the bytes, windy or not, and each next effect would add rows.
- **B. A block of its own** (`cbuffer Wind`), bound only for the `WIND` variants. **Chosen** (user
  2026-10-29): wind has room to grow there (gusts, a scene wind direction, a material's response,
  bending away from what walks through) without touching any shared block; still draws carry and
  see nothing; and a binding that exists only in a variant's pipelines is a mechanism the next
  vertex effect can reuse.

### Wind's parameters belong to the effect, not to each shader (proposed 2026-10-29)

With its own block, wind's three values no longer need to live in the material's shader buffer, so
they need not be declared by each shader either (the opt-in by the name `WindStrength` goes). The
user asked that this not be a C++ field on the material (a `WindSettings` beside `blendMode` would
make the material type grow with every effect, and its editor rows hand-written). Instead the
effect declares its parameters where its code is:

- **`wind.hlsli` declares an effect block**, in the syntax of a custom shader's `properties`:
  `effect Wind { float Strength = 0 [0, 2]; float Speed = 1 [0, 20]; float Height = 1 [0.01, 100]; }`.
  The cook parses it with the properties parser and generates `cbuffer Wind` at the effect's binding
  in place of the block, as it does `cbuffer Material` for `properties`.
- **A material whose shader declares the effect's variant** (`WIND` in its vertex variants: PBR,
  Phong, Unlit, the surface shaders' generated programs, a low-level shader that lists it) **gets the
  effect's parameters as a group of its own**, stored apart from the shader's properties and never
  in the shader's buffer. Its strength above zero is the opt-in (`MaterialWantsWind` reads the
  group, not a property named `WindStrength`).
- **The editor's material page** shows the group as a section ("Wind"), its rows from the
  declaration as a custom shader's properties are (`MaterialPage.cppm`'s `MaterialPropertyRowFor`);
  a material whose shader cannot sway has no such section.
- **The renderer** fills the effect's block from the group, by the declared layout, and binds it for
  the `WIND` variants (B).
- **The next vertex effect** is an include with its own `effect` block and variant: no C++ type, no
  material field, no editor code. What the engine knows is the mechanism (an effect block tied to a
  variant flag), and `WIND` stays in the flag table as it is today.

What exists to build on (read 2026-10-29):

- **The parser** (`Foundation/Shaders/Properties.cppm`): `ParseShaderProperties` finds the block by
  the keyword `properties` (`FindBlockStart`) and builds the table an effect needs (names, types,
  defaults, ranges, offsets by `PackCbufferMember`); `GenerateMaterialDeclarations` writes
  `cbuffer Material : register(b0, space2)`. Both become parameters (the keyword; the block's name
  and binding). An effect takes uniforms only at first (no textures).
- **The material's storage** (`MaterialSource`, `Materials.Resource/MaterialResource.cppm`; the
  asset, `Pipeline/Materials.Pipeline/MaterialAsset.cppm`, version 3): one flat property set as
  parallel arrays (`propertyNames`, `propertyTypes`, `propertyOffsets`, `propertySizes`,
  `uniformDefaults`, the texture slots). An effect group needs the same description per effect;
  either a list of effect groups beside the shader's set, or the shader's set made the first of a
  list of groups. Either way a data version bump with a migration (a PBR material's three wind
  lanes become its Wind group's values).
- **Vegetation** is what wind was made for (`terrain-vegetation.md`, Decision 5; commit b834ae390):
  a layer's instanced set draws with one material, so one Wind block per material serves the set.
  No sample's material turns wind on today.

What follows from it:

- **`wind.hlsli` stops declaring the material buffer**; its effect block becomes `cbuffer Wind`, and
  `WindSway()` reads it and calls `WindSwayOf`. The `#ifdef WIND` sites in the three stages stay.
- **The clock leaves its borrowed lanes** (`IblParams.zw` for forward, `ShadowWind.x` for the
  shadow, `WindTime` for the pick) for named `Time` and `PrevTime` fields of each pass's view block
  (`shader-blocks-cleanup.md`, phase 2), which the wind and surface shaders' `Time()` both read.
- **Every shader that builds on the engine's vertex stage can sway** by declaring `WIND` in its
  vertex variants: PBR, Phong, Unlit, the surface shaders' generated programs, a low-level shader
  that includes `forward_vs.hlsli`. It declares no wind properties of its own.
- **The shadow and the pick follow the mesh for every material**; `DepthLikeFor` binds the Wind
  block for a windy draw and stops binding the material set for wind (a masked cut-out still binds
  it).
- **PBR's three wind lanes** are migrated into its materials' Wind groups and removed from its
  buffer (the material data version bump above).

Checked against the alternatives:

- **Every shader declares the wind fields in its own material buffer, read by name.** Fixes
  forward for a custom shader, but the shared shadow and pick programs still cannot know a custom
  layout; each custom shader would need its own depth and pick programs. Rejected.
- **A wind component on the entity, not the material.** Moves the authoring away from the
  material, where the editor's page and the opt-in already are; and a card's sway is a property of
  what it is made of. Not needed for this.

## To verify before building

- How the three passes' binding layouts take a binding present only in their `WIND` variants (the
  pipeline layout per variant), on Vulkan and WebGPU.
- Where a material's Wind block lives: one uniform per windy material, built from its group and
  rebuilt when it changes; how an entity's material instance overrides it.
- WGSL alignment of the Wind block (scalars or a float4, not a float3).

## Phases

1. **The effect block.** The properties parser takes the keyword and the generated block's name and
   binding; `wind.hlsli` declares `effect Wind`; the cook generates `cbuffer Wind`. Tests: the
   parser on an `effect` block (table, errors, generated declaration); a shader with `WIND` compiles
   in every variant.
2. **The material's Wind group.** Stored apart from the shader's properties (a data version bump; a
   PBR material's three lanes migrate into it); given to a material whose shader declares `WIND`;
   shown as a section on the material page. Tests: the migration; a Phong, an Unlit and a custom
   material get the group, a shader without `WIND` does not; the page's section.
3. **The draw carries it.** The renderer builds each windy material's Wind block and binds it for the
   forward, shadow and pick `WIND` variants; `MaterialWantsWind` reads the group; the clock moves to
   the view blocks; PBR's buffer loses its wind lanes. Tests: the existing wind tests (renamed to the
   group); a probe drawing a Phong and a custom material with wind, their forward, shadow and pick
   all swaying as PBR's does, on Vulkan and WebGPU.
4. **Docs.** The custom shaders guide (`Documentation/Shipping/McpGuide.md`, the docs repo's shader
   page) and the vegetation docs: how a shader opts in (`WIND` in its vertex variants) and where a
   material's wind is set.
