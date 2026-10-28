# Custom shaders - a game's own surfaces, written in the editor

> Status: DONE 2026-10-12: phases 0 to 6 built (the phase 6 hand-over is the user's to look at). The showcase
> (7) is the Shader Gallery sample (`Specs/shader-gallery.md`, user 2026-10-12); the disassembly view and project includes (8) are in
> backlog-0.2. Spiked:
> `Code/Samples/CustomShader` (2026-10-12) proves the low-level kind at the engine level. Two
> kinds are supported (user, 2026-10-12): low-level shaders and surface shaders.
> Track: was the 0.1 backlog's "Custom shaders through the editor" (closed 2026-10-12).
> Mirror: Sedulous ports each phase after it lands.
> Read CONVENTIONS.md first.

## Where things stand

**At the engine level, low-level custom shaders work as designed** (747cba0f, "material-driven
set-2 layout + PSO": "a custom shader with a different property set gets its own set-2 layout +
PSO with zero renderer changes (the foundation for custom shaders like toon)"). The
`CustomShader` sample proves it, ported from legacy Sedulous's `RenderMaterialsCustom` (BeefGFX):
a toon shader registered by name (`ShaderSystem::RegisterSource`), a material built in code whose
properties, in order, lay out the shader's set-2 block, and nothing in the renderer told about
it. It draws distinct and instanced meshes, a masked material, the skinned Fox, receives the
sun's cascades and a point light's cube shadow, and writes the motion vectors TAA reprojects
with. The contract a low-level shader keeps: the mesh vertex in, `forward_vs.hlsli`'s VSOutput
(or its own equivalent), sets 0, 1 and 3 as `lighting.hlsli` declares them, its own set
2, the SKINNED / INSTANCED / WIND and ALPHA_TEST / GBUFFER / DITHER variants, and under GBUFFER
the five targets. It forks the whole lighting; that is the point of this kind.

What does not exist is everything between that and a user of the editor or a shipped game:

- **Authoring.** A `ShaderAsset` (`Pipeline/Shaders.Pipeline/ShaderAsset.cppm`) is a name plus
  two HLSL files, vertex and fragment, each with a `main`: the low-level kind's shape. Nothing
  creates one (no Create entry; a `.hlsl` file imports as nothing), it opens in the generic form
  page, and a material's parameters can only be declared in code (`MaterialBuilder`).
- **The cook copies text.** `ShaderAssetBuilder` reads both files into a `ShaderSource` and never
  compiles them, so broken HLSL cooks "successfully". Included files are not tracked.
- **Errors surface at draw time.** The first draw compiles the variant through DXC; a failure is
  `Shader variant compile failed: ...` in the log, every frame (not cached), and the old modules
  and pipeline are already gone, so the mesh vanishes. The hot-reload doc's "keep the old on
  failure" is not what the code does.
- **Exported games cannot run one.** A dist ships no DXC; a registered source logs "cannot be
  compiled in compiler-free pack mode" and draws nothing. Only the engine corpus (`Data/Engine/Shaders`)
  is precompiled, to SPIR-V, DXIL and WGSL (DXC, then naga and tint for the web).
- **Depth, shadow and pick passes use built-in shaders.** A custom vertex shader's displacement
  never reaches them, and masked shadows assume `AlbedoMap` at t0.
- **The material page** has a read-only Shader row, and its preview ignores `shaderId`.
- **No include asset exists** (the backlog says there is one). Includes resolve against
  `Data/Engine/Shaders` in dev builds only (the toon shader includes `forward_vs.hlsli` that way).
- The engine's own depth and shadow pipelines draw with Vulkan validation warnings ("Vertex
  attribute at location 1..4 not consumed by vertex shader"), in every sample with shadows: a
  position-only shader on the full mesh layout. Harmless, but noise to fix on the way.

## Two kinds, for two purposes

**Low-level shaders** (what the asset holds today, and what the toon sample is). Full control: a
whole shading model of one's own (toon, a stylised water, a hologram), at the price of keeping the
frame's contract above, and following it when the renderer changes. For engine-literate users
and effects the surface model cannot express.

**Surface shaders.** The user writes what makes the surface theirs, and the engine writes the
rest:

```hlsl
// A lava surface: scrolling, glowing cracks.
properties
{
    Color   CrackColor = (1.0, 0.35, 0.05, 1.0);
    float   Speed      = 0.2   [0, 2];
    float   Glow       = 4.0   [0, 20];
    Texture Cracks;
}

void Surface(SurfaceInput input, inout SurfaceOutput surface)
{
    float2 uv = input.uv + float2(0.0, Time() * Speed);
    float crack = Cracks.Sample(MainSampler, uv).r;
    surface.albedo = lerp(float3(0.08, 0.05, 0.04), CrackColor.rgb, crack);
    surface.roughness = 0.9;
    surface.emissive = CrackColor.rgb * crack * Glow;
}

// Optional: move the vertex (world space), e.g. waves.
// void Vertex(inout VertexInput input) { ... }
```

The engine wraps it in its own templates: the forward vertex shader (skinning, instancing, wind),
the lighting and IBL, the five targets and velocity, the depth and shadow passes. `SurfaceOutput`
carries albedo, alpha, normal (tangent space), metallic, roughness, occlusion and emissive; a
`lit` or `unlit` model is declared at the top. What the user gets for free: every light, shadow,
probe, post-processing pass and TAA works, skinned and instanced meshes work, and a renderer change
updates every user shader with it. This is what Unity's surface shaders and Godot's shader
language give, and it is exactly what a node graph would generate later (tier 2), so the two
tiers meet instead of competing.

Both are shader assets, and a material picks either the same way. They share everything below
the authoring: compiled at cook time, shipped as bytecode, the editor's shader page, the material
page's picker and rows. A surface shader is a low-level shader the engine writes for you.

### Properties: declared in the shader

The `properties` block is the one place a parameter is declared, in either kind, and in either kind
the engine generates from it the material cbuffer (16-byte packed), the texture and sampler
declarations at the set-2 slots `MaterialSystem` assigns, and the material's property table: names,
types, defaults, ranges for the editor's sliders. No reflection of compiled shaders is needed, and
the layout cannot drift from the code because both come from one declaration. Types: `float`,
`float2..4`, `int`, `Color` (sRGB, linear in the shader), `ColorHdr` (with an intensity, as the PBR
emissive is), `Texture` (2D), `TextureCube`, `Sampler` (declared explicitly). A low-level shader
carries the same block (it replaces the `MaterialBuilder` calls the toon sample makes in code) and
declares only the frame's sets, never set 2. A material on either shows exactly these rows;
switching a material to another shader keeps the values whose names and types match.

## How it compiles, ships and reloads

- **Compiled at cook time.** The shader asset's builder assembles template plus surface, compiles
  every variant the templates use (vertex: SKINNED, INSTANCED, WIND; pixel: ALPHA_TEST, GBUFFER,
  DITHER; the depth/shadow pair) for the running editor's backend, and fails the cook on an error.
  Errors are mapped back to the user's file and line (the generated code carries `#line`
  directives), so the page and the Console say `Lava.hlsl:12: undeclared identifier 'Sped'`.
- **Last good kept.** A failed cook keeps the last good product, as scripts do, so a broken edit
  never makes the mesh vanish; the runtime compiles only what a cook already proved compiles.
- **Shipped as bytecode.** Export cooks each project shader for the preset's formats (SPIR-V,
  DXIL, WGSL through the same naga/tint step) into the dist's pack, beside the engine corpus. A
  dist still carries no compiler, and the web works. A material's shader joins its dependencies,
  so a pruned export keeps it.
- **Hot reload.** Saving recooks and the editor reloads the product (the path materials and
  textures use today); dependent materials follow through the recorded edges. Pipelines swap only
  once the new variant exists.
- **Includes, for 0.1, are the engine's library** (`#include "surface/noise.hlsli"` and similar,
  shipped with the templates); project `.hlsli` files with dependency tracking come after.

## The editor

- **Create > Materials > Lit Surface Shader / Unlit Surface Shader**: a starter `.hlsl` in
  `Sources/`, linked to a new shader asset, opened in its page. Importing a `.hlsl` file makes one
  too.
- **The shader page**, on the script page's pattern: a code editor with an HLSL lexer (the C-like
  lexer already has HLSL's `#` lines; it needs keyword tables), completion for the template's
  names (`SurfaceInput`, `SurfaceOutput`, `Time()` (the view's clock, already in `IblParams.z`),
  the declared properties), errors checked as
  you type (a compile without saving, debounced) shown as markers, a list and a status line, and
  Save to cook. Beside it a live preview: the material page's sphere (or cube, plane, a project
  mesh) drawn with the shader and the properties' defaults.
- **The material page**: the Shader row becomes a picker (the built-in PBR and Unlit, and the
  project's shaders); its rows come from the shader's properties; the preview resolves the shader
  asset (today it ignores it); it refreshes when the asset changes outside the page.
- **Create > Materials > Material from Shader...**, or picking a shader on an existing material.
- **Disassembly** (the backlog's "nicety"): a toggle on the shader page showing the cooked
  SPIR-V or WGSL of a chosen variant. Small, and last.

## Agents (MCP)

- `shader_validate`: compile a source without saving, as `script_validate` does, returning errors
  with lines and the parsed properties.
- Creating a shader is `asset_create` with the new creators; setting a material's shader and its
  values is `asset_data_write` today. A `material_set_shader` tool if the envelope edit proves
  awkward (a finding, fixed in the engine).
- McpGuide gains a Shaders workflow; `kEngineToolCount` is bumped.

## The showcase

A sample of its own (user, 2026-10-12, replacing the fifth sample's showcase): the Shader Gallery,
eleven commented lesson shaders in a hall to fly through (`Specs/shader-gallery.md`). Plus a user docs page, "Custom
shaders", with the lava example.

## Phases (each a layer with its tests, both compilers; Sedulous ports after each)

Low-level shaders first: the engine already draws them, so each phase carries them one step
closer to an editor user and a shipped game. Surface shaders then build on the same asset path.

0. **The spike, and entry points as data** (done 2026-10-12): `Code/Samples/CustomShader`, the
   toon shader in `Data/Assets/shaders/toon/toon.hlsl`, one file with `VSMain` and `PSMain`, kept
   as the low-level reference sample. Entry names are carried end to end (user, 2026-10-12: what
   is correct over what is convenient): a source registers each stage's entry, the compiler,
   WGSL translation and pack (v3) carry it, every module records it, and a pipeline stage runs
   the entry it names or its module's; no pass writes `main`. Still owed on the way: the built-in
   depth and shadow pipelines' "attribute not consumed" validation warnings.
1. **The shader asset compiles at cook time.** The low-level asset (its stages, its `properties`
   block) cooks every variant for the editor's backend, fails the cook with errors mapped to the
   file and line, keeps the last good product, and carries the property table; a material on it
   takes its layout from that table, and set 2 is generated from it. A
   failed runtime compile is cached, not retried every frame. Tests: a good shader cooks its
   variants; a broken one fails with its line and the old product stays.
2. **Export.** Project shaders cooked to the preset's formats (SPIR-V, DXIL, WGSL) into the dist
   pack; the player resolves them compiler-free; the material-to-shader dependency survives
   pruning. Tested on a desktop dist and the web build.
3. **Editor** (built 2026-10-12). `CheckShaderSource` (`shaders.pipeline:check`):
   what the cook proves, as data (file, line, column, message), which the cook, the page and
   the MCP tool share. The HLSL lexer (`editor.shaders`, "hlsl" and "hlsli"). Create > Materials
   > Shader (a short starter on the lighting library, not the toon: the toon forks the lighting,
   which is the advanced case) and a `.hlsl` importer, each naming the shader after its asset.
   The shader page: the code, checked a pause after typing on each stage's base variant (Save
   cooks every variant), errors as markers, a list and a status line; a sphere drawn with the
   text being edited under a name of the page's own; the name and entry points. The material
   page: the Shader row picks a shader asset or, cleared, a built-in (PBR or Unlit), as one
   undoable edit that keeps every value whose name, type and size still fit
   (`CarryMaterialValues`); its rows are the shader's, with the block's ranges and rows for
   float2, float3 and int; it follows the shader when it recooks; its preview draws with it.
   The hand-over (2026-10-12): a scratch project with the toon imported as a `.hlsl` and the
   starter, a material on each, exported for desktop and the web, for the user to run. Running
   the desktop dist found a phase 2 gap: a Debug player carries DXC, so a project shader's
   stripped (empty) source compiled and failed; an empty registered source is now served from
   the pack whether or not a compiler is present.
4. **Agents and docs** (built 2026-10-12). `shader_validate` (the cook's check over every
   variant; errors with file, line and column; the parsed properties and each stage's variant
   flags), the include path from each host's `EngineToolPaths::shaderLibrary`, `kEngineToolCount`
   31; McpGuide's Shaders workflow; the docs site's "Custom shaders" page.
5. **The lighting library** (done 2026-10-12, ahead of phase 3 so the editor's starter is
   short). `view.hlsli` (the View constants, once), `lighting.hlsli` (lights, shadows, IBL,
   probes, clusters; `DirectLight`, `AmbientLight`, and `LightsAt`/`LightAt`/`ShadowOf` for a
   shader that shades lights its own way) and `forward_ps.hlsli` (`GBufferOutput`,
   `ScreenDoorFade`, `OctEncode`, `MotionVector`). `forward.ps.hlsl`, `unlit.ps.hlsl` and the toon
   include them; the toon fell from 344 lines to 102. Verified on the compiled corpus: every WGSL
   entry is the old one but for SSA names and one constant initialised a line earlier; the
   vertex stages are byte-identical; unlit's View struct gained the two trailing members it never
   reads. One shape matters to that: a helper that returns early from inside a branch makes DXC
   lay the code out differently, so the helpers keep the original branch and a single return.
6. **Surface shaders.** The templates around `Surface()` and `Vertex()`, the generated
   declarations with `#line` mapping, generated depth/shadow/pick variants (so displacement and
   cut-outs shadow correctly); the built-in PBR re-expressed as a surface function renders the
   same as `forward`. The editor's creators gain Lit and Unlit Surface Shader; the docs page gains
   the lava example.
7. **The showcase**: the Shader Gallery sample (`Specs/shader-gallery.md`).
8. After 0.1: the disassembly view and project includes; the node graph generating surface
   functions is 0.2.

## Phase 1 in detail: the shader asset compiles at cook time

Planned 2026-10-12 from the code (no build). Layers in order, each with its tests; the editor
page and export are phases 3 and 2.

**1a. Errors name the file.** `CompileOptions` gains `sourceName`, passed to DXC as the source's
file name, so a message reads `Toon.hlsl:12:5: error: ...` instead of DXC's placeholder name.
Test: a broken source compiled under a name reports that name and the line.

**1b. The properties block** (`Foundation/Shaders`, a parser beside the variant directives).
```hlsl
properties
{
    Color    BaseColor   = (1, 1, 1, 1);
    Color    ShadowColor = (0.3, 0.1, 0.1, 1);
    float    Bands       = 3       [1, 8];
    ColorHdr Glow        = (1, 0.5, 0, 1);
    Texture  AlbedoMap;
    Sampler  MainSampler;
}
```
One declaration per line: a type (`float`, `float2`, `float3`, `float4`, `int`, `Color`,
`ColorHdr`, `Texture`, `TextureCube`, `Sampler`), a name, an optional default and an optional
`[min, max]` range for the editor's slider. Parse errors carry the line. The parser yields the
property table in declaration order. The cbuffer layout follows HLSL's packing rules, through
ONE packer in `foundation.shaders` that `MaterialBuilder` uses too: a value is aligned to its own
size and never crosses a 16-byte row. (`MaterialBuilder` packs a `float3` as 16 bytes and lets a
`float2` straddle a row, where HLSL packs a following `float` into the `float3`'s last lane and
moves the `float2` to the next row: a latent mismatch with what a shader reads. Nothing calls its
`Float2` or `Float3` today, so no layout changes.) Textures and samplers are numbered t0.., s0..
in order, as `MaterialSystem::GetOrCreateLayout` assigns them. Tests: each type and its default,
ranges, the packer against HLSL's rules (float3 then float, float2 at 12), `MaterialBuilder` on the
same list, every malformed line reported with its line number.

**1c. Set 2 is generated from the block, in both kinds** (decided 2026-10-12). The block is
replaced in place by the generated `cbuffer Material : register(b0, space2)` and the texture and
sampler declarations, followed by `#line <next line> "<file>"`, so every later line keeps its
number in DXC's messages. A low-level shader then declares sets 0, 1 and 3 (the frame) and never
set 2. Test: the generated declarations compile, and an error after the block reports the line
the author sees.

**1d. Per-stage variant directives.** A one-file shader declares each stage's variant lattice:
`// variants(vertex): SKINNED INSTANCED WIND` and `// variants(fragment): ALPHA_TEST GBUFFER
DITHER`. The corpus's untagged `// variants:` keeps meaning every stage of its file. The drift lint
checks a one-file shader against the union of its stages' lattices (which code belongs to which
stage is the preprocessor's to know): a flag used anywhere must be declared for some stage.
Tests: tagged and untagged forms, a flag used but not declared fails.

**1e. The shader asset's new shape.** `ShaderAsset`: `name`, `fileName` (the one `.hlsl`),
`vertexEntryPoint` (default `VSMain`) and `fragmentEntryPoint` (default `PSMain`), visible and
editable in the asset's inspector. `fragmentFile` goes (no project uses the two-file shape); the
data version is bumped.

**1f. The cook compiles.** `ShaderAssetBuilder` reads the file, parses the block, generates set 2,
and compiles every declared variant of both stages to SPIR-V with the engine's shader library on the
include path (`<data root>/Shaders`: every tool passes the data root it resolved, `--data-root`
included, to `RegisterAllBuilders`). Any failure fails the cook with `File.hlsl:line: message` lines
in the log; the cook driver keeps the last good product, so a broken edit never reaches a running
game. The product (`ShaderSource`) carries the generated source, both entry points, both declared
masks and the property table. Builder version 3. Tests: a good shader cooks with its table; a broken
one fails with its file and line and the previous product survives; an undeclared flag fails the
cook.

**1g. The runtime registers what the cook proved.** `ShaderFactory` registers each stage with its
entry point and declared mask, so requests canonicalize as the corpus's do (and as the dist
lookup will in phase 2). A runtime compile failure is cached per variant until the shader is
invalidated, instead of recompiled and logged every frame. Tests: canonicalization of a
registered source with a mask; a failing variant compiles once.

**1h. Materials take their layout from their shader.** `MaterialAssetBuilder`: when `shaderId` is
set, it declares a `reads` dependency on the shader (a shader edit re-cooks its materials), reads
the cooked `ShaderSource`'s table, lays the product out by it, and takes each value from the asset
by name where the type matches, else the shader's default. A value whose name the shader no
longer has is dropped with a warning. Built-in materials (`shaderName`) are unchanged. Tests: a
material on a shader gets its layout and values; a shader edit that adds, removes and retypes
properties re-cooks the material as described.

**Out of phase 1:** project `.hlsli` includes (engine library only), the editor's shader page and
material picker (phase 3), bytecode in dists (phase 2).

## Phase 2 in detail: project shaders ship as bytecode

Planned 2026-10-12 from the code (no build). Today a dist resolves only the engine corpus from its
pack (`CookEngineShaders`, at export); a project's shader reaches a dist as source, which a
compiler-free player refuses ("cannot be compiled in compiler-free pack mode") and draws nothing.

**2a. The pack cooker cooks any shader stage.** `CookOne` takes the stage's entry point instead
of `kCorpusEntryPoint`, and a public `CookShaderStage(compiler, translator, name, stage, source,
entryPoint, mask, sourceName, includePaths, formats, pack, report)` records the stage's lattice
and entry in the pack and cooks every (variant, format) blob: SPIR-V and DXIL through DXC, WGSL
through naga and tint. The corpus loop calls it per file. Test: the toon's two stages cook to
SPIR-V and WGSL under `VSMain` and `PSMain` (tint accepting them is the web build's proof).

**2b. Export cooks the project's shaders into the dist pack.** After the content cook, every
`ShaderSource` the dist ships (all of them, or the reachable ones when pruning: a material `reads`
its shader, so the closure already keeps it) is cooked for the preset's formats into the same pack
as the corpus, with the engine library on the include path. A failure fails the export, naming
the shader's file. Tests (Integration.Mcp export): a project with a custom shader and a material
on it exports; the dist pack holds the shader's variants and entries in the preset's format; a
pruned export keeps them.

**2c. A dist resolves them without a compiler.** `ShaderSystem::GetVariant`: a registered source
compiles when a compiler is present (the editor, dev); without one the pack serves it, registered
or not, with the pack's entry and lattice; only a variant the pack lacks is the loud cook-coverage
error. Test: a compiler-free system with a registered source and a pack holding it gets the pack's
module, entry and canonicalization.

**2d. A custom shader cannot take an engine shader's name.** One name keys a shader in the pack
and in the shader system, so a project shader named `forward` would silently replace the engine's.
The cook refuses a name the engine library already uses (a `<name>.vs/ps/cs.hlsl` in
`<data root>/Shaders`). Test: the refusal, with its message.

**2e. Ship bytecode, not source, unless the preset asks** (decided 2026-10-12): the dist's
`ShaderSource` products are written without their HLSL (the pack carries the shader), as the
engine corpus ships none. An export preset's "Include shader source" (`includeShaderSource`, off
by default; `export_preset_set` takes it, so McpGuide names it) keeps it, for a game that wants
modders to read its shaders. Export
packs the cooked files byte for byte (`PackTree`, from the project's `Cooked/` and a staging
folder), and the project's own products must keep their source for the editor, so the shader
products are left out of the raw copy and a stripped copy of each is staged at the same path.
Tests: an exported product has empty sources and keeps its entries, lattices and property table;
the project's own product is untouched; with the preset's switch on, the source ships.

**2f. Hand-over for a look: moved to phase 3.** A scratch project with the toon on a few meshes,
exported for desktop and for the web, for the user to run. No project can make a custom shader
until phase 3 brings the Create entry and the `.hlsl` importer, so it waits for them. (The
showcase proper is the Shader Gallery's.)

Built 2026-10-12: 2a to 2e. The export test covers an unpruned export; a pruned one keeps a
material's shader through the cook's closure, which follows `reads` (the closure's own tests), not
through a test of its own.

## Phase 6 in detail: surface shaders (plan, 2026-10-12)

**What the engine does today, that this builds on** (surveyed 2026-10-12). No material supplies
its own depth, shadow or pick shader: every mesh caster goes through `shadow_depth` (the camera
depth prepass, the cascades, the local-light atlas) or `pick_ids`, chosen by name in
`ShadowConfigFor`/`PickConfigFor` (`MeshRendererImpl.cpp`), with SKINNED, INSTANCED, ALPHA_TEST
and WIND variants. A masked caster cuts at a hard-coded 0.5 on set 2's first texture, which is
the albedo only for the built-in layouts; WIND reads the forward PBR cbuffer at fixed offsets.
So a vertex displacement or a cut-out of a custom shader never reaches the shadows, and a
low-level custom shader with a different first texture shadows wrongly when masked. Those depth
and pick pipelines also leave four vertex attributes unread (the owed Vulkan warnings).

**The authoring.** A surface shader is a shader asset whose `.hlsl` says so on a directive line,
like the variant lines: `// surface: lit` or `// surface: unlit`. It holds a `properties` block,
`Surface()`, and optionally `Vertex()`:

```hlsl
// surface: lit
properties { ... }
void Surface(SurfaceInput input, inout SurfaceOutput surface) { ... }
void Vertex(inout VertexInput v) { ... }   // optional: world space, before projection
```

- `SurfaceInput`: world position, world normal and tangent, uv, vertex colour, view direction,
  screen position. `SurfaceOutput`: albedo, alpha, normal (tangent space), metallic, roughness,
  occlusion, emissive, starting at the defaults of a plain white surface.
- `VertexInput`: world position, normal and tangent, uv, colour; `Vertex()` may move the position
  and turn the normal and tangent. It runs in every pass, so displacement shadows, picks and
  writes depth where the surface is drawn. `Time()` is the view's clock in every pass (forward:
  `IblParams.z`; depth and pick: their view's time), and the previous frame's clock where the
  forward pass computes motion vectors, so a moving surface reprojects.
- The variants are the template's, not the author's: SKINNED, INSTANCED, ALPHA_TEST, GBUFFER and
  DITHER are compiled for every surface shader; no `variants` lines. ALPHA_TEST cuts at the
  material's `AlphaCutoff` when the block declares one (0.5 otherwise) on `surface.alpha`.
- Lit shades with `DirectLight` and `AmbientLight`; unlit writes albedo plus emissive. Both write
  the five GBUFFER targets.

**The cook.** The builder sees the directive and assembles three programs from one file, each
`#include`ing the engine's templates around the author's text with `#line` kept, so an error
names the author's file and line: the forward program (`surface/forward.hlsli`), the depth
program (`surface/depth.hlsli`: the shadow view's set 0, the Vertex hook, under ALPHA_TEST the
whole `Surface()` for its alpha) and the pick program (`surface/pick.hlsli`). `Vertex()` is
optional: the cook finds it in the text and defines `SURFACE_HAS_VERTEX`. Each program compiles in
every variant, through `CheckShaderSource`, which gains the surface mode, so the page, the cook
and `shader_validate` keep one check.

**A shader with several passes.** `ShaderSource` carries a list of programs (a name suffix,
both stages' source, entries and lattices): a low-level shader has one, a surface shader three
(`<name>`, `<name>.depth`, `<name>.pick`). The factory registers each; the export cooks each into
the pack; the product's version bumps. A material on a shader with its own depth program records
the names (`Material::depthShaderName`, `pickShaderName`; empty = the built-in), and
`ShadowConfigFor`/`PickConfigFor` use them with the material's set 2, as the masked path does
today. A surface shader without `Vertex()` that is opaque keeps the built-in depth (no set 2,
shared pipelines); with `Vertex()`, or masked, it uses its own.

**Proof.** The built-in PBR written as a lit surface shader (a test asset, wind in its `Vertex()`)
renders as `forward` does: compared on the cooked corpus as phase 5 was (WGSL modulo names), and
where the code order differs, by a GPU image of both side by side in Render.Backend.Tests.

**Built** (2026-10-12): layers 1 to 5 below. On the way: a surface property named like an engine
declaration (`Tint`, in the vertex stage) is reported at the property's line (a property is a
global name in the generated code); `VertexInput` carries the mesh's local position (forward's
wind needs it, so `wind_sway.hlsli` holds the sway apart from the PBR cbuffer); the pass vertex
stages became libraries (`shadow_depth_vs.hlsli`, `pick_ids_vs.hlsli`) with the hook in an
`#ifdef`, the engine's corpus byte-identical; the forward family's pixel stages keep their inputs
(`preserve-interface`), ending the last validation warnings. The proof measured identical images
on Vulkan and WebGPU (`Integration.Shaders`).

**Layers**, each with its tests:
1. The depth and pick passes per material: `depthShaderName`/`pickShaderName`, the config
   functions using them, `preserve-interface` on `shadow_depth.vs`/`pick_ids.vs` (the owed
   warnings).
2. `ShaderSource` with several programs: factory, export and pack.
3. The surface templates and the cook's surface mode (`CheckShaderSource` with it).
4. The PBR as a surface shader, proven against `forward`.
5. The editor: Create > Materials > Lit Surface Shader and Unlit Surface Shader (the lava as the
   lit starter); the existing creator renamed Low-Level Shader; the docs page's surface section
   and McpGuide.
6. The hand-over: a scratch project with the lava and a displaced (waves) surface casting
   shadows, desktop and web, for the user.

## Decided

- **Two kinds, both supported** (user, 2026-10-12): low-level shaders and surface shaders.
- **One `.hlsl` per shader, with named entries** (`VSMain`, `PSMain`), and entry names carried as
  data rather than assumed (user, 2026-10-12). Built (phase 0).
- **The order**: low-level shaders through the editor and into dists first (phases 1 to 4), then
  surface shaders (5, 6) (user, 2026-10-12).
- **The properties block** is the single declaration of a shader's parameters, in both kinds
  (user, 2026-10-12).
- **The showcase is a sample of its own**, the Shader Gallery (user, 2026-10-12; it was to go in
  the fifth sample).
- **The node graph is 0.2** (user, 2026-10-12; `backlog-0.2.md`).
- **Set 2 is generated from the properties block in both kinds** (user, 2026-10-12): a low-level
  shader declares the frame (sets 0, 1, 3) and never its material block, so the layout cannot
  disagree with the block and the cook needs no reflection. A hand-packed material block is not
  supported.
- **Entry points are asset fields**, defaulting to `VSMain` and `PSMain` (user, 2026-10-12).
- **Samplers are declared explicitly** in the block (user, 2026-10-12): the material layout is
  exactly the block.
- **A dist ships no shader source** by default (user, 2026-10-12); an export preset's "Include
  shader source" keeps it.
- **A surface shader declares itself** on a directive line, `// surface: lit` or `// surface:
  unlit`, like the variant lines (user, 2026-10-12).
- **Its own depth, shadow and pick programs only when needed** (user, 2026-10-12): with
  `Vertex()` or on a masked material; an opaque one without `Vertex()` keeps the built-in depth.
- **The creators**: Lit Surface Shader, Unlit Surface Shader, and the existing one renamed
  Low-Level Shader (user, 2026-10-12).

## Corrections this plan owed other documents (made 2026-10-12)

- backlog-0.1.md said "custom shaders already work at the engine level" and named "shader include
  assets": the item is closed, its leftovers moved (the showcase to a sample of its own, the
  Shader Gallery, the disassembly view and project includes to backlog-0.2).
- shaders-materials-hot-reload.md described a binding-signature hash and interface validation
  that were never built: it now says what reloads do.
- shaders.md said nothing of a custom shader in a dist: its custom shaders section covers dists
  and surface shaders.
