# Shader blocks cleanup - every value in a field named for it

> Status: APPROVED 2026-10-29; phases 0 (f1e5416e1), 1 (9cf6a7739) and 2a (the clock) BUILT on clang;
> 2b (wind as an effect) next; gcc and the other lanes owed (user: "just slotting unrelated things in
> unused blocks"; "I'd like if we could clean up all those shader things slopped in").
> The wind's own design is `wind-for-every-shader.md`, phase 2 here.
> Mirror: Sedulous ports each phase after it lands.
> Read CONVENTIONS.md first.

## Why

The engine's shader constant blocks carry many values in lanes named for something else, in
padding, or packed several to a vector, and a shared include declares one material's layout. Each
was a small saving when it went in; together they make the blocks hard to read, the C++ mirrors
wrong in their comments, and one feature (wind) usable by PBR alone. An audit on 2026-10-29 read
every block under `Data/Engine/Shaders/` and the C++ that fills it: 26 findings, listed below with
the phase that removes each.

## The rules

1. **Every value has a field named for it**, in the shader and in the C++ mirror. A vector is
   named for all its parts or holds one thing (`ShadowCamera: xyz = position` is fine; `IblParams.zw
   = the clock` is not).
2. **Padding is padding.** It is never written with a meaning nor read. A block grows when it gains
   a value (respecting WGSL's alignment: scalars or a float4, never a float3 followed by a scalar
   that the rules would move).
3. **A shared include declares no one material's or pass's block.** It reads values by name that
   the including shader provides.
4. **A block of one pass is not described as padding in another.** Where passes share a binding,
   each declares the fields it reads by their names, or the passes get blocks of their own.
5. **Every block has a C++ mirror struct** (no anonymous `f32 push[20]`), with `static_assert`s on
   its size and its fields' offsets, and a test that compares it with the shader's declaration.
6. **Data of one draw or one set lives in a per-draw or per-set block**, not in a private copy of
   the view block.

## The layout test (phase 0)

The engine has no shader reflection today. The test compares each engine shader block with its C++
mirror by one of:

- **DXC reflection**: compile the shader as the cook does and read the constant buffer's members and
  offsets (`IDxcUtils::CreateReflection` on the DXIL). Exact; needs DXC, which the tests already
  have on the dev lanes, not in the wasm build.
- **The declaration's text through the engine's own packing**: parse the `cbuffer` and lay it out
  with `PackCbufferMember` (the HLSL packing the custom shaders' properties already use and test).
  No compiler; trusts the packing code.

Ruled 2026-10-29 (user): a unit test, in a suite of its own, `Integration.ShaderBlocks` (it compiles
the engine's shaders and reads the renderers' structs, and needs no GPU, which `Integration.Shaders`
is skipped without); the web build does not run it. It reads the SPIR-V that DXC
compiles (each block's `OpMemberName` and `OpMemberDecorate ... Offset`), not DXC's reflection
interface, whose header (`d3d12shader.h`) needs DirectX-Headers and its Windows stubs on Linux
(user, "Yes", 2026-10-29): the same compile the cook runs, the offsets Vulkan and the WGSL
translation use, and no new third-party code. Either way: one table naming each
block (shader file, block name, C++ struct), so a new block cannot go without its mirror.

## The findings and the phase that removes each

Paths under `Data/Engine/Shaders/` and `Code/`. Categories: (a) a lane of a field named for
something else; (b) padding or a "spare" lane given a meaning; (c) unrelated values packed into one
vector; (d) one material's or pass's layout in a shared include.

### Phase 1: the instance fade, one named form in every pass

A faded instanced set (vegetation's distance fade) carries its window and each instance's rank in
four borrowed places, in a different form per pass:

- **F2** `View.ShadowParams.zw` = the set's fade window, in a private copy of the view block per set
  (`MeshRendererImpl.cpp:1749`), `ShadowParams.w > 0` as the "faded" flag (a).
- **F6** `InstanceData.Tint.a` = the instance's rank `(i + 0.5) / count` (`MeshRendererImpl.cpp:789`);
  the real alpha is lost for a faded set (a).
- **F8, part** `ShadowView.ShadowWind.yz` = the window, `.z > 0` the flag (c).
- **F9, part** `PickView.FadeStart` and `PickCamera.w` = the window, `PickCamera.w > 0` the flag (a).

The change: the set's window (`FadeStart`, `FadeEnd`) as named fields of a per-set block (the
`Object` block of the set's draw, or a block of its own), the same in the three passes; the rank
computed in the vertex stage from the instance's index and a named `InstanceCount`, not uploaded in
the tint; the tint keeps its alpha. The private view-block copies per set go.

Built 2026-10-29: `cbuffer InstanceSet` (instance_fade.hlsli; `render::GpuInstanceSet`), set 1
binding 1 beside the instances in the forward, shadow and pick passes: `FadeStart`, `FadeEnd`,
`InstanceCount` and the set's pick id (`SetPickIndex`/`SetPickGeneration`, which left the pick
view's private slot with the window, rule 6). A MultiMesh set owns one per frame-in-flight region,
written each frame it draws; a per-frame instanced batch binds a constant one (no fade, each
instance its own id). The rank is `(index + 0.5) / InstanceCount` in the vertex stage; the tint
keeps its alpha. The shadow view says by a named `ShadowFadeFromCamera` whether its pass couples
to a camera (a pass with none never faded); `ShadowCamera` and `PickCamera` became float3. The
private view-block copies per set, and their budgets (`kMaxFadedSetSlots`, `kMaxPickMultiMeshSets`),
are gone. Tests: the existing
`Render.Tests/MultiMeshLifecycleTests.cpp:348` (rank in the tint) is replaced by one on the named
fields; `Render.Backend.Tests/InstanceFadeProbeTests.cpp` (GPU) passes unchanged.

### Phase 2: wind and the clock

`wind-for-every-shader.md` (a wind block of its own, bound only for the `WIND` variants: B, ruled
2026-10-29), plus the clock:

- **F10** the PBR material buffer's `WindStrength`, `WindSpeed`, `WindHeight` in its spare lanes,
  offsets 24, 28, 60 (`MaterialBuilder.cppm:208`) (b).
- **F11** `wind.hlsli` declares PBR's `cbuffer Material` for every stage that includes it; Phong's
  and Unlit's `WIND` variants would read PBR's offsets from their own buffers (d).
- **F1** `View.IblParams.zw` = the clock, this frame's and last frame's (`MeshRendererImpl.cpp:1030`),
  read by the wind and by surface shaders' `Time()` (a).
- **F8, part** `ShadowView.ShadowWind.x` = the clock (c).
- **F9, part** `PickView.WindTime` = the clock, in the row after `SetPickId`.

The clock becomes named `SceneTime` and `PrevSceneTime` fields of each pass's view block, which
the wind and surface shaders' `Time()` both read. Built 2026-10-29 (2a): the forward view's
`IblParams` is four named scalars (`IblDiffuse`, `IblSpecular`, `SceneTime`, `PrevSceneTime`), the
shadow view's `ShadowWind` a named `SceneTime`, the pick view's `WindTime` `SceneTime` (not `Time`:
that is the surface API's `Time()`, which a field of the same name hid). The PBR wind lanes stay in its
material for now, read by the renderer by name; removing them is a data version bump, phase 6.

### Phase 3: the per-draw block and the instance stream

- **F5** forward's and the shadow's `Object._objPad` = the pick pass's `PickIndex`/`PickGeneration`
  (`MeshRenderer.cppm:320`); one C++ `ObjectData` is the union of three passes' layouts, each
  shader calling the others' fields padding, and the shadow and pick blocks truncated before
  `Fade` (b, d-like).
- **F7** the `DataOffsets` vertex stream: `.z` = the previous bone base (forward) or the
  generation (pick), `.w` = the fade's float bits (forward) or the entity index + 1 (pick)
  (`MeshRendererImpl.cpp:1406`, `:1660`) (a, c).

The change: each pass's per-draw block declares every field it reads by its name (one C++ struct
whose fields all three shaders name, or a struct per pass); the stream gets named fields per pass.

Built 2026-10-29: `cbuffer Object` is declared once, in `object_block.hlsli` (`render::GpuObject`),
which the forward, shadow depth and pick vertex stages include in their non-instanced branch: every
field by its name (`PickIndex`, `PickGeneration`, `Fade` in all three) and honest padding
`_objectPad0..2` (the C++ `_fadePad[3]` became `_objectPad0..2`, the same bytes). The stream is two
C++ structs with offset asserts, `GpuDrawOffsets` (instance index, bone base, previous bone base,
the fade as an `f32`) for the forward and depth fills, the MultiMesh ramp and a skinned set's
offsets, and `GpuPickDrawOffsets` (instance index, bone base, pick generation, pick index) for the
pick fill; the shaders call it `drawOffsets` (forward, depth; w read with `asfloat`) and
`pickOffsets` (pick). A MultiMesh set's pick draw binds a stream in the pick layout too, ids 0 (it
picks as its `InstanceSet`'s entity): a pick ramp beside the forward's, and a skinned set's pick
offsets filled beside its forward ones (it bound the forward's at first, the pick shader reading
the previous bone base and the fade as an id it then ignored). Location, format and stride are
unchanged. The renderer also frees a live set's InstanceSet and pick buffers at shutdown. Tests: `Integration.ShaderBlocks` checks one `Object`
table in the forward, shadow depth, pick, Phong and Unlit vertex stages.

### Phase 4: probes and lights

- **F3** `View.ProbeCenter`: `.x` = the scene's base index into `Probes[]`, `.w` = the count, `.yz`
  unused; nothing is a centre (`MeshRendererImpl.cpp:995`) (a). Becomes `ProbeBase`, `ProbeCount`.
- **F4** `View.ProbeBoxMin`/`ProbeBoxMax`: documented, never written nor read (dead). Removed.
- **F13** `GpuProbe`: `center.w` = intensity, `boxMin.w` = blend distance, `boxMax.w` = cube slice,
  `params` = mip count, priority, parallax, with the C++ mirror calling `params.zw` padding while
  `params.z` is written (`ReflectionProbeSystemImpl.cpp:111`) (b, c). Named fields.
- **F12** `GpuLight` lanes 14 and 15 named `pad0`/`pad1` in `cluster_build.cs.hlsl:18`, which carry
  `shadowIndex`/`shadowStrength` (b). Named as in `lighting.hlsli`.

Built 2026-10-29: the view's probe lanes are `ProbeBase` and `ProbeCount` (uints, with honest
padding; the dead `ProbeBoxMin`/`ProbeBoxMax` are gone, so `GpuView` is 656 bytes), and `GpuProbe`
names every scalar (`center`, `intensity`, `boxMin`, `blendDistance`, `boxMax`, `slice`,
`mipCount`, `priority`, `parallax`) in the same 64 bytes and order, so the uploaded bytes do not
change. F12 was fixed in phase 0. `Render.Tests` checks a probe record's values by name; no GPU
test draws reflection probes.

### Phase 5: sky, image lighting, terrain, effects, screen passes

- **F14, F15, F16, F17** the image-lighting precompute push (`ibl_common.hlsli`, no C++ assert):
  `Horizon.a` = sun intensity, `Zenith.a` = sky rotation, `Ground.a` = turbidity (undocumented),
  `Sun.w` = angular size (a, c). Named fields.
- **F18** sky `SunDir.w` = the sun's angular size in degrees (a); `CamPosIntensity` (borderline c).
- **F19, F20** terrain: `ShadowParams.z` = height-blend contrast, `.w` = height maps bound (with
  `terrain.vs.hlsl` still calling `zw` spare); `SplatParams` = palette count, weights flag, base tile
  scale, base flag (a, c); `ViewUBO` and `ChunkUBO` without asserts, `kViewSlotSize`'s comment
  wrong.
- **F21** decal `Params`: xy = 1 / target size, zw = the fade's cosines (c).
- **F22, F23** particles: `DepthParams.w` documented as the soft fade but written 0 (dead);
  `SizeRotMode.w` = the soft distance while the C++ mirror says unused; one size split across
  `PositionSize.w` and `SizeRotMode.x`; `Velocity.w` = the stretch (b, c).
- **F24** sprites: width in `PositionSize.w`, height and mode in `SizeOrientation` (c); the C++
  comment's size stale.
- **F25** tonemap: `Rotation` in a former pad, the C++ side an anonymous `f32 push[20]` still
  calling it padding (b). A struct.
- **F26** SSGI resolve `SkyAmbient.w` = the SH sky dimmer, 0 meaning no SH sky (c, mild).

### Phase 6: what the phases leave

- PBR's three wind lanes removed from its material (a data version bump and migration), once
  phase 2 reads wind by name.
- The VG vertex attributes (section 16 of the audit): the box shadow's `TexCoord`/`Coverage`
  carrying the SDF operand and corner radius or sigma; `Effect` meaning an outline in one pipeline
  and saturation and brightness in another. Named per pipeline.
- Out of scope, kept and documented: the TAA history target's alpha holding the previous depth (a
  texture channel, not a constant block).

### Stale comments and missing mirrors (fixed in phase 0, built 2026-10-29)

`ViewData`'s size comments (560, 528; it is 688) and its "spare" notes; no `static_assert` on
`ViewData`, terrain `ViewUBO`/`ChunkUBO`, `SkyUniform`, `IblPush`, `ClusterBuildParams`, the
particle view, `VGUniforms`, `VGBlurUniforms`, the bloom, TAA, AO and debug blit pushes, and
tonemap.

Built: every engine block above has a named mirror at namespace scope (`Gpu<Block>`, beside the
pass that fills it), a size `static_assert`, and a row in `Integration.ShaderBlocks`, which reads
the blocks from the SPIR-V the engine cook compiles (1802 checks); the anonymous arrays (tonemap,
FXAA, exposure, the debug pushes) became structs filled field by field with the same bytes; the
material buffers of PBR, Phong and Unlit are checked against `MaterialBuilder`'s offsets; the
cluster kernel's light lanes are named as lighting's. Skipped: the ImGui extension's `Proj`, and
Phong's and Unlit's `WIND` variants (they declare PBR's buffer, F11, phase 2).

## Order and cost

Phase 0 first, so every later phase lands against a test that its two sides agree. Phases 1 and 2
are the cluster that most of the borrowed lanes belong to (the instance fade and the wind spread
over the view, object, instance and material blocks of three passes); then 3, 4 and 5, each a
commit or a few, every change on both backends (Vulkan and WebGPU, whose WGSL alignment rules the
new fields follow), the GPU probes passing unchanged. Blocks grow by a few float4s at most; no
pass's work changes.
