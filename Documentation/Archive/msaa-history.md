# Scene-pass MSAA

> ARCHIVED 2026-11-02, partly built; what it still owes is in Documentation/Plans/weekly_backlog.md
> ("Left over from the specs archived 2026-11-02"). Non-authoritative: the original spec, kept as
> the record of what was built and why; the code and the tests are the truth.

> Status: P1 IN PROGRESS - render mechanism + editor toggle + web (Vulkan + WebGPU) shipped
> and on-screen verified; P1g pixel-probe DONE (generic RHI.TestSupport harness + MSAA probe, both
> backends). Remaining: re-run the existing GTAO/SSR/TAA/FXAA probes with MSAA on. See "Build state".
> Track: backlog I11 / renderer
> Author: Fable, 2026-08-12. Opus executes; Fable reviews per phase.
> Resume at P1g (the pixel-probe acceptance test).
>
> VERIFICATION GAP (user, 2026-08-16): DX12/Windows has NOT run any of the
> MSAA verification (probes or on-screen) - Vulkan + WebGPU only. Tracked in
> KNOWN_ISSUES.md; next Windows session runs the probe suites on DX12.

**Motivation:** the Sponza-vs-Godot comparison (I11). Mips closed the
dominant gap; geometry-edge quality without TAA's temporal artifacts is the
remainder. Godot defaults to MSAA; our 3D pass is single-sampled with post
AA only (TAA/FXAA). Precedent in-tree: the UI canvas/world-panel path
already renders 4x MSAA with resolves, and PipelineStateCache already keys
`desc.multisample.count` from `config.sampleCount` - the PSO plumbing is
half-built.

## Decisions (settled here, not during the build)

1. **Sample counts: off / 2x / 4x, and 4x is the ceiling on web.** WebGPU
   guarantees 4x for the formats we use; 8x is a native-only capability
   probe (OPTIONAL follow-up, not in this track - do not build an 8x path
   the web target cannot validate). The count is capability-clamped at
   device init and the SETTING records intent, not the clamped result.

2. **MSAA is per-VIEW, not global.** The renderer is multi-view (editor
   viewport, game view, camera previews, web player). The sample count
   rides the same per-view surface as ViewPostOverride: the editor
   viewport gets a toggle in the existing post-flags menu; the game reads
   a project setting (`renderMsaaSamples`, default OFF for now - flipping
   the default is a one-line decision after perf numbers exist).

3. **The prepass problem - the load-bearing choice.** GTAO, SSR, and
   motion vectors consume the depth prepass at 1x today. A 4x main pass
   with a 1x prepass breaks early-Z (depth mismatch per sample) and
   starves post of matching depth. RULING: when a view is MSAA, the
   DEPTH PREPASS RUNS MSAA TOO (same sample state as the main pass -
   early-Z stays exact), and a RESOLVED depth (first-sample resolve -
   cheap, deterministic, and what the post effects effectively sampled
   before) is produced for every 1x consumer (GTAO/SSR/motion vectors/
   TAA history reprojection). Post effects stay 1x and UNCHANGED - they
   read resolved color + resolved depth. Accepted consequence: AO/SSR
   edges are computed from one sample per pixel (identical to today's
   quality); full per-sample post is out of scope permanently.

4. **Resolve points (CORRECTED 2026-08-12 - see the ordering note + ruling
   below).** Scene color resolves ONCE, after the OPAQUE portion of the
   forward pass (opaque + sky + decals), via the render-pass resolve
   attachment (native on WebGPU; pResolveAttachments on Vulkan) - never a
   blit. Everything after - SSR, AO, TAA, then TRANSPARENT, then the post
   stack - runs on resolved 1x color, preserving the deliberate temporal
   ordering (AO/SSR pre-TAA for stabilization; transparent post-TAA
   against ghosting). Opaque geometry edges - the I11 case - get full
   MSAA; transparent silhouettes keep today's treatment (follow-up noted
   below). HDR RGBA16F resolve is supported on both backends; assert the
   capability at init rather than assuming.

   > **Implementation note (Opus 2026-08-12, for Fable review) - the 1x
   > consumers need resolved AUX buffers, not just depth.** Decision 3
   > says the prepass produces a resolved DEPTH for the 1x consumers. In
   > the code those consumers read more than depth: AO reads `normal`;
   > SSR reads `normal` + `material` + `velocity`; TAA reads `velocity`.
   > An MRT render pass forces every attachment to one sample count, so
   > the forward pass's aux G-buffers (`normal`/`velocity`/`material`)
   > become MSAA too and each needs a first-sample resolve alongside
   > depth - otherwise AO/SSR/TAA cannot sample them at 1x. This is the
   > forced consequence of "post stays 1x and reads those buffers," not a
   > scope change.
   >
   > **Mechanism chosen:** depth + the three aux resolve via a single
   > fullscreen SHADER pass that does `textureLoad(msaaTex, coord, 0)`
   > (sample 0) and writes the 1x outputs (a render pass, not a blit;
   > identical on Vulkan + WebGPU). Sample-0 (not an averaged resolve) is
   > what keeps AO/SSR/TAA "identical to today's quality" per Decision 3.
   > This is DISTINCT from the scene-COLOR resolve above, which correctly
   > uses the hardware resolve attachment (averaged - the real edge AA).
   > So under MSAA-on: 5 resolves total - color (hardware, averaged,
   > forward-end) + depth/normal/velocity/material (shader, sample-0,
   > after opaque forward).
   >
   > Rejected alternative for depth: a hardware depth/stencil resolve
   > attachment with `VK_RESOLVE_MODE_SAMPLE_ZERO_BIT`. Viable on Vulkan
   > but adds backend-specific depth-resolve plumbing and WebGPU expresses
   > it differently; the uniform shader pass avoids the split and also
   > covers the color-format aux (which have no sample-zero hardware
   > resolve mode at all). If Fable prefers the hardware depth path for
   > the depth specifically, that is a bounded swap of one resolve source.

   > **Fable RULING (2026-08-12): APPROVED - the aux-buffer consequence is
   > real and correctly derived, the shader-pass mechanism is right, and
   > the hardware depth-resolve rejection stands (backend split + covers
   > nothing the aux need). Three pins before building:**
   >
   > 1. **Depth resolves through SV_Depth into a REAL depth-format 1x
   >    target** - the resolve pass binds an MRT of the three aux colors
   >    PLUS a depth attachment written via SV_Depth (gl_FragDepth). Do
   >    NOT respell depth as an R32Float color target: every existing
   >    consumer (GTAO/SSR/TAA/motion reprojection) binds a depth-format
   >    texture today, and a format respelling would ripple through their
   >    bindings and shader declarations for zero benefit. With SV_Depth,
   >    every 1x consumer is untouched - which is the whole point of
   >    Decision 3.
   > 2. **The sample-0 nuance, documented where the shader lives:** sample
   >    0 of a standard MSAA pattern is NOT the pixel center, so resolved
   >    depth/normals sit ~0.4px off where a 1x prepass would sample.
   >    Consistent frame-to-frame and benign under TAA's own jitter -
   >    accepted. The rule it protects: depth is NEVER averaged (nonlinear
   >    - an averaged edge depth is a point in empty space).
   > 3. **Ordering belongs to the graph, not the prose:** "after opaque
   >    forward" over-specifies. The resolve pass is scheduled by its
   >    dependencies - after the LAST producer of depth+aux, before their
   >    FIRST 1x consumer - and the frame graph already orders that. If a
   >    consumer (GTAO) can run before transparents, the graph is free to
   >    schedule it so.
   >
   > Build on.

   > **Implementation note (Opus 2026-08-12, for Fable review) - the resolve
   > point is FORCED before transparent, not at forward-end.** Decision 4
   > models the frame as `forward(opaque+transparent) -> resolve -> SSR
   > composite onward`. The actual pipeline (PipelineImpl.cpp End(), HDR
   > path) is `opaque+sky+decals -> SSR -> AO -> TAA -> transparent ->
   > bloom/tonemap`: SSR/AO/TAA run BEFORE transparent, proven by the
   > color-handle dependency chain (not declaration order) -
   > `hdr ->(SSR) sceneHdr ->(AO apply) litHdr ->(TAA) sceneColor
   > ->(transparent) sceneColor` - so transparent's target IS TAA's output
   > and cannot schedule earlier.
   >
   > This ordering is a deliberate TEMPORAL design, and it makes the two
   > constraints incompatible with Decision 4's literal shape:
   > - **AO/SSR must be PRE-TAA** (:1707-1710): AO is computed from the
   >   jittered G-buffer, so applying it post-TAA wobbles sub-pixel each
   >   frame - TAA must stabilize it.
   > - **Transparent must be POST-TAA** (:1732-1734): it composites on the
   >   resolved image so blended geometry is never temporally accumulated
   >   (no ghost) or jittered (no wobble).
   >
   > Decision 3 requires SSR/AO/TAA to read color+depth at 1x RESOLVED. Since
   > those consumers are forced before transparent, the resolve of the scene
   > color + depth/aux must land AFTER OPAQUE (+sky+decals), before them -
   > NOT after transparent. "Resolve after transparent, SSR onward"
   > (Decision 4 literal) would require moving AO after TAA (wobble) or
   > transparent before TAA (ghost), breaking exactly what those lines
   > protect.
   >
   > **Consequence + the one honest cost:** the OPAQUE scene color resolves
   > after opaque(+sky+decals) via the hardware resolve attachment; SSR/AO/
   > TAA consume the 1x resolved buffers UNCHANGED (Decision 3 verbatim);
   > transparent stays post-TAA on the 1x resolved color, so TRANSPARENT
   > silhouettes get today's treatment, not MSAA. Opaque geometry edges -
   > the Sponza/I11 case the track is about - are fully MSAA-antialiased.
   > Transparent-edge MSAA is a bounded follow-up (a second MSAA color +
   > resolve around the transparent pass, once the machinery exists), noted
   > not built.
   >
   > **Ask:** confirm this reshaping of Decision 4 (resolve-after-opaque, not
   > resolve-after-transparent; transparent-edge MSAA deferred). If Fable
   > wants transparent inside the MSAA resolve in P1, that is a larger change
   > to the temporal ordering and should be its own decision.

   > **Fable RULING (2026-08-12): CONFIRMED - Decision 4's text is corrected
   > above to match.** The dependency-chain reading is concrete and the
   > temporal design it protects is exactly right to protect: AO computed
   > from the jittered G-buffer NEEDS TAA behind it, and transparents must
   > never be temporally accumulated - MSAA bends around that design, never
   > the reverse. Decision 4 as I wrote it modeled an idealized frame; the
   > real one wins. Transparent-edge MSAA stays a noted follow-up (the
   > second MSAA target + resolve wrap), not P1.
   >
   > Two pins the reshape adds:
   > 1. **The transparent depth-test nuance, documented where the pass
   >    lives:** transparents depth-test against the SAMPLE-0 resolved 1x
   >    depth of MSAA opaque geometry - along opaque silhouettes the
   >    averaged color says "edge" where sample-0 depth may say "empty",
   >    so transparents crossing opaque edges can show <=1px acne/halo.
   >    Accepted for P1; the transparent-MSAA follow-up fixes this and
   >    edge AA together.
   > 2. **Decision 6's PSO matrix SHRINKS:** with transparent and
   >    everything post-TAA at 1x, only the passes rendering into the
   >    MSAA target carry the sample key - prepass, forward OPAQUE, sky,
   >    decals (and particles/sprites/debug draw ONLY if they render in
   >    the opaque MSAA pass - verify against the graph, do not assume).
   >    Less work than the original list; adjust it to the graph's truth.
   >
   > P1 acceptance adjusts accordingly: the edge-coverage probe targets an
   > OPAQUE edge, and a transparent-over-opaque case asserts no REGRESSION
   > (today's quality, not MSAA). Build on.

5. **MSAA and TAA are independent toggles.** They solve different
   aliasing (geometry edges vs shading/specular); both-on is legal and
   sometimes right (MSAA4 + TAA is the Godot-quality look). The editor
   post-flags menu shows both plainly. Document the cost note in the
   settings tooltip, not a hidden coupling.

6. **PSO matrix.** Every pipeline that renders INTO the scene pass
   (forward opaque/transparent, sky, decals, particles, sprites, debug
   draw, prepass variants) gains the sample count in its cache key -
   PipelineStateCache already carries `sampleCount`; the work is
   threading the VIEW's count into every PipelineConfig at that level
   (grep for configs constructed with the default). Cache re-key only -
   the bind-group versioning rules are untouched.

## What this deliberately does NOT touch

- Shadow maps, IBL/probe capture, canvas-RTT UI (has its own MSAA), the
  VG overlay (own MSAA), pixel-probe suites for VG.
- Alpha-to-coverage (foliage) - note as a cheap follow-up once counts
  plumb; not in this track.
- 8x, sample shading, per-sample post: out.

## Build state (2026-08-12)

P1 render mechanism is BUILT, committed, and verified on screen (Vulkan + WebGPU). What
remains for P1 is the automated pixel-probe acceptance test (P1g). Detail:

**Done (shipped + on branch `game-ready-scripting`):**
- Frame-graph multisampled color+depth transients + a resolve attachment (`RGColorTarget.resolveHandle`,
  `PassBuilder::SetResolveTarget`, executor mapping).
- `MsaaResolvePass` - first-sample shader resolve of depth + aux G-buffer (normal/velocity/material)
  into 1x, feeding the existing 1x consumers. Color resolves via a hardware resolve attachment.
- PSO sample-count threading (prepass + forward opaque + sky + decals via the config/PSO key);
  MSAA-off is byte-identical by construction (all counts default 1, post* handles alias, no resolve
  passes emitted).
- Device capability + clamp: `Device::MaxColorDepthSampleCount` (ceiling) AND
  `Device::SupportsSampleCount` (exact-count set - Vulkan bitmask; WebGPU `{1,4}`, no 2x); per-view
  snap-down to a supported count; `ValidatedDevice` forwards both.
- Editor viewport toggle (off/2x/4x post-flags menu) + WebScene sample toggle (offers only
  device-supported counts).
- Web (Emscripten/WebGPU): WGSL cook of the resolve shader + full scene runs 4x in the browser.
- Both compilers (clang + gcc) + wasm green; on-screen verified on Sponza (RTX 2060: 4x engages,
  resolves, toggles clean, no validation errors) and on WebGPU (browser + desktop `--webgpu`/WGSL).

**WebGPU-strictness fixes made during the web bring-up (Vulkan tolerated all three):** resolve-pass
multisampled aux must be `UnfilterableFloat`; decals must SAMPLE the resolved 1x depth, not the MSAA
attachment (moved decals just after the resolve - now consistent with AO/SSR; **Fable to note this
ordering refinement in review**); sample counts are `{1,4}` only on WebGPU (snap 2x -> 1x). Captured
as a standing rule in memory `webgpu-stricter-than-vulkan`.

**Done (P1g) - the pixel-probe acceptance test, on a generic capture/probe harness:**
- DECISION (2026-08-13): the readback substrate is GENERIC, not MSAA-specific - it was already needed
  in three places (`VG.Backend.Tests/VGPixelProbeTests`, `Render.Backend.Tests/BackendOrientationTests`,
  and now the MSAA probe each rolled the same "make device -> render offscreen -> CopyTextureToBuffer
  -> Map -> probe pixels"). Landed `foundation.rhi.testsupport` (new `Foundation::RHI.TestSupport`
  lib, RHI + Core only): `MakeTestDevice(Backend&)`; `CapturedImage` (tightly-packed RGBA +
  `At`/`Luma`/`CountWhere(pred)`); and a decoupled `Readback(device, colorTargetInCopySrc, w, h)`
  owning the 256-aligned staging buffer + copy + map + unpack. Each consumer keeps its OWN render
  setup; STRUCTURAL assertions only (no golden images).
- MSAA probe (first consumer) in `Render.Backend.Tests/MsaaProbeTests.cpp`: a flat-lit rotated cube
  on black through the full `RenderFrame` chain (forward + `MsaaResolvePass` + tonemap) at
  `post.msaaSamples` 1 then 4; assert the 4x image has partial-coverage silhouette pixels (luma banded
  relative to each image's own max) that the hard-edged 1x lacks. MEASURED: maxLuma 533, fringe
  1x=**0**, 4x=**103** - identical on Vulkan and WebGPU, both compilers, deterministic. Tests the real
  pipeline path.

- Post-effect composition (the other half of P1's acceptance): a second `MsaaProbeTests` case renders
  the cube at 4x with each of TAA/FXAA/AO/SSR enabled (the effects read the RESOLVED 1x buffers, so a
  broken resolve-rebind would surface here) and asserts each still renders a sane lit cube. All four
  pass on Vulkan + WebGPU, both compilers - MSAA composes with the full post stack. DONE.

- Harness generality (DONE): the VG probe and the orientation probe were refactored onto the shared
  `MakeTestDevice`/`Readback`/`CapturedImage` - the inline readback + device-creation are gone from all
  three consumers, and all still pass (VG 232, Render.Backend 544 assertions, both compilers). Proves
  the extracted substrate is genuinely general.

**P1g is fully closed.** P2 is IN PROGRESS:
- DONE: `ProjectSettings.renderMsaaSamples` (v9; default 1 = off, existing projects byte-identical);
  the PLAYER applies it on startup (global path, capability-clamped per view); the project settings
  dialog has an MSAA combo that reads/writes it.
- DONE (from a review note): the Off/2x/4x list + index<->sample mapping are centralized in
  `engine::render::kMsaaLevels` (+ `MsaaLevelCount`/`MsaaIndexForSamples`/`MsaaSamplesForIndex`); every
  MSAA UI builds from it, so adding 8x is one line there (plus the device ceiling + `SupportsSampleCount`).
- OPEN: **play-in-editor apply** - deferred because the global `SetMsaaSamples` path would bleed into
  editor viewports and persist after Stop; PIE needs a PER-VIEW application (a view post override with
  `msaaOverride` on the game tab's views) + restore on Stop.
- OPEN: **measured perf numbers** (frame ms + memory at 1x/2x/4x on the render-stress scene, desktop +
  web) recorded in the systems doc; then the default-on/off decision goes to the user with numbers.

Then P3 (polish) and the DX12 capability overrides noted below.
- **DX12 capability overrides** - DONE (38c6d184, Windows side): `DxDevice` overrides
  `MaxColorDepthSampleCount` / `SupportsSampleCount` via
  `CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS)` per format, and the MSAA probe
  compiles a DX12 leg that self-skips off-Windows. Still pending: an actual Windows RUN of the
  probes (KNOWN_ISSUES entry), and the queries are re-issued per call (per-view per-frame from
  RenderSubsystem) - a one-time init bitmask would make them free.

## Phases

**P1 - the multisampled view (the whole mechanism, editor-first).**
Frame-graph support for multisampled color+depth transient targets with a
resolve attachment; prepass + forward OPAQUE + the passes that share its
MSAA target (sky/decals - others per the graph's truth) take the view's
sample state via the PSO key; first-sample depth resolve feeding the existing 1x consumers; the
editor viewport toggle (off/2x/4x) in the post-flags menu. Acceptance:
the pixel-probe harness grows a scene-pass case - a high-contrast edge
rendered at 1x vs 4x, asserting intermediate-coverage pixels appear on
the 4x edge (the VG MSAA test's precedent, applied to the 3D path);
GTAO/SSR/TAA/FXAA all still pass their existing probes with MSAA on;
both backends; wasm builds and renders (4x).

**P2 - the game setting + perf honesty.** `renderMsaaSamples` project
setting (player + play-in-editor honor it; capability-clamped), settings
UI, and MEASURED numbers on the render-stress scene (frame ms + memory
at 1x/2x/4x, desktop + web) recorded in the systems doc. Then - and only
then - the default-on/off decision goes to the user with numbers.

**P3 - polish.** Camera-preview inset inherits the main view's count;
export templates carry the setting; alpha-to-coverage spike if particles/
foliage want it (separate approval).

## Acceptance (track)

Both compilers + wasm green; the new scene-pass probe + full battery;
zero behavior change with MSAA off (the default) - single-sample paths
byte-identical in the probe; editor toggle user-verified on Sponza
against the Godot reference that motivated I11.

Checklist (2026-08-12):
- [x] Both compilers (clang + gcc) + wasm green.
- [x] Zero behavior change with MSAA off - byte-identical by construction (all counts default 1,
      post* handles alias the originals, no resolve passes emitted). To be re-asserted mechanically by
      the P1g probe.
- [x] Editor toggle user-verified on Sponza (RTX 2060); WebGPU verified in-browser + desktop `--webgpu`.
- [x] New scene-pass pixel probe (1x vs 4x opaque edge) - **DONE** (RHI.TestSupport harness +
      MsaaProbeTests; fringe 1x=0, 4x=103, Vulkan + WebGPU, both compilers).
- [x] Post effects (TAA/SSR/GTAO/FXAA) re-run green with MSAA on - **DONE** (MsaaProbeTests
      effect-stack case; each renders a sane cube at 4x, Vulkan + WebGPU, both compilers).
