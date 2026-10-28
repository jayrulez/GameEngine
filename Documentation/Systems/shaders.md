# Shaders - out of C++, cooked per backend

> Status: CURRENT
> Verified: 2026-08-12 @ fd25f6de
> Track: [[shaders-track]]

Engine shaders are HLSL source `.hlsl` files (not C++ string banks), compiled on demand by runtime DXC
in dev and cooked to per-backend bytecode for export. Shipped: the source lift (P1), cook-time bytecode
(P2), and the WGSL translation target (P3). The dedicated Shader editor page (P4) is the one deferred
piece.

## Decisions

- **HLSL is the authoring language** (the whole ported corpus is HLSL; DXC is the front end). WGSL is a
  cook-time TRANSLATION target, not a source language.
- **All engine built-ins live as `.hlsl` files** in the data root's `Shaders/` folder, read through the
  application's data mount (`Documentation/Systems/data-root.md`), sharing code via `.hlsli` - served
  to DXC by the file provider as the compiler's include resolver (no native include paths). Passes
  fetch by name. The old C++ string-bank partitions are deleted.
- **Dual-mode cooked form**: dev/editor keeps runtime-DXC compile-on-demand (instant hot reload, no
  variant pre-enumeration); EXPORT cooks declared variant sets to per-backend bytecode - SPIR-V (Vulkan)
  / DXIL (DX12) / WGSL (WebGPU). A shipped dist drops the DXC sidecar entirely.

## Modules

- **`foundation.shaders`** (`Code/Foundation/Shaders`, `+ /Cook`) - the dlopen'd DXC `Compiler`, the
  `ShaderPack` cooked format (`ShaderPackCooker`), and the `WgslTranslator`.
- **`foundation.shaders.resource`** (`Shaders.Resource`) - `ShaderResource` / `ShaderFactory` /
  `ShaderSource` / the `ShaderSystem` variant registry.
- **`foundation.shaders.system`** (`Shaders.System`) - the host that picks dev-compile vs pack mode
  over the data mount (`ShaderSystemHost`: `Shaders/` sources, or the cooked `Shaders/shaders.dpak` -
  `kShaderPackPath`) + `FileShaderSourceProvider` (the hot-reload source AND the include resolver).
- **`shaders.pipeline`** (`Pipeline/Shaders.Pipeline`) - the cook integration.
- **`Tools.ShaderPack`** - the CLI shader-pack cooker.

## Depth convention

`Data/Engine/Shaders/depth.hlsli` is the shader-side twin of `core::projection` / `rhi::depth`: REVERSE-Z
(near = 1, far = 0, cleared = 0, nearer = larger). A shader that tests for background, walks depths
to the closest, biases a shadow compare or linearizes explicitly includes it and names its helpers
(`IsBackgroundDepth`, `NearerOf`, `BiasTowardViewer`, `LinearizeDepth`); one that reconstructs
position through an inverse projection is convention-free. See renderer.md 8.1.

## The forward library

The forward pass is built from includes a custom shader uses too (`Specs/custom-shaders.md`):

- `view.hlsli`: the frame's View constants (set 0, b0), the one declaration of their layout.
  Mirrors the CPU view block.
- `forward_vs.hlsli`: the whole vertex stage (skinning, instancing, wind, the motion-vector clip
  positions), `main` unless the includer names it (`#define FORWARD_VS_ENTRY VSMain`).
- `lighting.hlsli`: the lights, shadows, image-based light, reflection probes and clusters
  (sets 0 and 3) and the functions over them: `DirectLight` (every light reaching a pixel,
  Cook-Torrance shaded and shadowed), `AmbientLight` (IBL, probes and the flat ambient),
  `EvaluateLight`, `ShadowFactor`, and for a shader that shades lights its own way
  `LightsAt`/`LightAt` (the pixel's cluster list, or every light), `LightDirection` (direction
  and falloff) and `ShadowOf`.
- `forward_ps.hlsli`: the pixel stage's contract: `GBufferOutput` (the five targets),
  `ScreenDoorFade` (DITHER), `OctEncode` and `MotionVector`.

- `shadow_depth_vs.hlsli`, `pick_ids_vs.hlsli`: the depth and pick vertex stages, which
  `shadow_depth.vs.hlsl` and `pick_ids.vs.hlsl` are under `main`.
- `wind_sway.hlsli`: the wind sway as a function of its parameters (`wind.hlsli` binds it to the
  forward material's).

`forward.ps.hlsl` is these plus its material and its surface code; the split (2026-10-12) left
its compiled WGSL the same but for names and one constant's placement.

**Surface shaders** (`surface/`, `Specs/custom-shaders.md` phase 6). The cook assembles a
surface shader's three programs as `surface/surface.hlsli` (its API: `SurfaceInput`,
`SurfaceOutput`, `VertexInput`, `Time()`), the author's text, `surface/glue.hlsli` (the calls
into `Surface()` and `Vertex()`), and a pass template: `surface/forward.hlsli`,
`surface/depth.hlsli` or `surface/pick.hlsli`. The vertex libraries above run `Vertex()` where
`SURFACE_PROGRAM` is defined (in an `#ifdef`, so the engine's own shaders compile as before); a
material on a surface shader with `Vertex()`, or masked, draws the depth prepass, the shadows and
the pick with the shader's `.depth` and `.pick` programs (`Material::depthShaderName`).

## Variant model + cooked pack

RHI takes BYTECODE only (`ShaderModuleDesc.code` = SPIR-V / DXIL, or WGSL text; HLSL never crosses
the RHI boundary). `ShaderSystem` is a per-(name, stage) source registry + `ShaderFlags` (permutation bits ->
`#define`s) + compile-on-demand caching. Cook: shaders declare their meaningful variant subset of the
flag space; the cooker compiles each `(name, stage, variant, backend)` to a `ShaderPack` blob (SPIR-V /
DXIL bytecode, or WGSL text), binary + versioned + self-describing (carries the name table so the
runtime provider resolves by name). A shipped dist reads the pack and carries no DXC or naga.

**Entry points are data, not an assumption.** Every source is registered per (name, stage) with
the name of that stage's function in it (`RegisterSource(name, stage, hlsl, entryPoint)`; a
provider returns it with the text): one file may serve both stages as `VSMain` and `PSMain`, and
the engine's own corpus states its convention once, a file per stage whose function is `main`
(`kCorpusEntryPoint`). The compiler, the WGSL translation and the pack carry it (pack format v3
added a per-stage entry table; an older pack is refused and cooked again), and every module the
system creates records it (`ShaderModuleDesc::entryPoint` -> `ShaderModule::entryPoint`). A
pipeline stage runs the entry it names, else its module's (`rhi::EntryPointOf`; `rhi::StageOf`
builds a stage that runs the module's), so no pass or cache writes `main`; a stage with neither
is refused by every backend and by the validation layer.

Two source directives, parsed from the stage file by every compile site (the runtime system, the
pack cooker, the WGSL translator): `// variants: A B C` declares the flag lattice; `//
preserve-interface` keeps every declared stage input / output in the SPIR-V interface (DXC's
`-fspv-preserve-interface`; `-O3` otherwise strips unread inputs). The second exists for a
fragment stage that is paired with another shader's vertex module and declares that module's
full output struct while reading one member - terrain_depth.ps under HOLES, whose vertex stage is
the colour pass's `terrain` module so the prepass depth stays bit-identical. Vulkan matches a
stage interface by location and treats a shorter input list as an error (or, when the locations
happen to line up, a silent garbage read), never as a subset; the terrain holes probe runs with
validation ON and counts `rhi::vk::ValidationErrorCount()` around the render for exactly this.

## Custom shaders (a project's own)

A custom shader is a `ShaderAsset`: ONE `.hlsl` with a `properties { ... }` block declaring its
material parameters, of one of two kinds (`Documentation/Specs/custom-shaders.md`):

- **Low-level**: both stages written out (entry points as asset fields, `VSMain` / `PSMain` by
  default) with a variant lattice per stage (`// variants(vertex): ...`,
  `// variants(fragment): ...`), keeping the forward contract with the library above.
- **Surface** (`// surface: lit` or `unlit`): a `Surface()` and an optional `Vertex()`, which
  the cook wraps in the `surface/` templates as three programs (forward, `.depth`, `.pick`) with
  the templates' lattices.

The cook (`ShaderAssetBuilder`, through `CheckShaderSource`) replaces the block with the generated
set-2 declarations (the `Material` cbuffer by HLSL packing, `PackCbufferMember`, then the textures
and samplers), keeps the author's line numbers with `#line`, and COMPILES every program's every
variant with the engine's library (`<data root>/Shaders`) on the include path: a failure fails
the cook with `File.hlsl:line: message`, and the cook keeps the last good product. The product
(`ShaderSource`) carries each program's expanded source, entries and lattices and the property
table; the runtime registers each program with its lattices, so requests canonicalize as the
corpus's do, and a failed runtime compile is remembered until the source changes (or a reload).
A material on a custom shader is cooked by the shader's table (its order and offsets; values by
name where the type matches, else the shader's default) and re-cooks when the shader does; it
draws the depth-like passes with the shader's own programs when the shader has a `Vertex()` or
the material is masked.

**In a dist**: the export cooks every program to the preset's formats into the dist's pack beside
the corpus, and strips the products' HLSL (the preset's `includeShaderSource` keeps it); a dist
serves a registered shader from its pack when it has no compiler or the source is empty.

## WGSL target (naga)

`WgslTranslator` cooks HLSL -> (DXC, vulkan1.1 target env + engine Standard binding shifts) -> SPIR-V ->
(naga) -> WGSL, at cook time on the dev/CI host only. naga was chosen over tint by a spike over the full
corpus. `--keep-coordinate-space` is load-bearing (the WebGPU clip-space unification - see
[[web-render-fixes-and-flip]]). Export stages the WGSL pack for the Web platform; desktop stages the
SPIR-V/DXIL pack (see `Documentation/Systems/export.md`).

## Hot reload

Dev hot reload is a portable POLLED stat-sweep, NOT native OS watchers: `NativeFileSystem::AsWatchable`
exposes a change source (no inotify/RDCW plumbing - deterministic + portable), and
`FileShaderSourceProvider::PollChanges` stat-sweeps every ~60th call (~1s at 60fps, throttled because
the sweep is O(files)). The render subsystem calls `m_shaders->PumpReloads()` each frame and idles the
GPU on a reload. Details of the resource-swap + PSO-invalidation plumbing are in
`Documentation/Systems/shaders-materials-hot-reload.md`.

## Deferred

- **P4 - the Shader editor page** (editor page #9): a `CodeEditView` HLSL page with DXC diagnostics as
  Error/Warning markers, per-stage editing, and a preview. NOT built (no `Editor.Shaders` / `ShaderPage`
  exist yet); this is where the code editor's deferred HLSL lexer lands.

---

The P3 translator spike (tint vs naga comparison over the corpus) and the phase-by-phase history are in
`Documentation/Archive/shaders-design-history.md`.
