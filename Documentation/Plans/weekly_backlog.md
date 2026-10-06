# Weekly backlog - open items across weeks

> One file for everything seeded, carried or queued that has NOT been built. A weekly
> (`week-YYYY-MM-DD.md`, weeks start Saturday) records the work of ITS week; whatever it
> still owes at the next week start moves here, verbatim, with its original dating, so no
> weekly has to be re-read for open items and none grows past its week. Pull an item OUT
> of here into the current weekly when it is actually started; delete it here when done
> (the weekly of the week it shipped is the record).
>
> Created 2026-09-12 from the open sections of week-2026-09-05.md (which had absorbed
> week-2026-08-29, week-2026-08-22 and the archived roadmap/backlog folders).

## Queued 2026-10-06 (user, the web demos on a phone)

- **Touch drives the game UI**: the web shell takes touch (`Shell.Web/WebInput.cppm`, start / move /
  end / cancel on the canvas) and consumes it so the page does not scroll or zoom, which also stops
  the browser turning taps into mouse clicks; the game UI reads only mouse, keys and pads. So on a
  phone no menu can be pressed (Snowline's title cannot pick a course). Route touch into the UI as
  pointer input (a tap presses, a drag scrolls). Check what Sedulous does first.
- **Touch controls in the sample games**: the input map already binds touch (`TouchButton`, a screen
  region; `TouchStick`, a floating virtual stick), but no game uses it. Each game binds a stick and
  buttons (Snowline: the left half carves, zones on the right for jump, tuck and grab) and draws a
  light overlay showing them, shown only once a touch is seen.
- **Orientation on mobile**: held upright, a phone gives the canvas a portrait shape, and the games
  are landscape. Options: ask the browser for landscape (`screen.orientation.lock`, which needs
  fullscreen and is refused on some browsers), letterbox a landscape render into the portrait canvas,
  or rotate the presented image a quarter turn with the input transformed to match, and a prompt to
  turn the phone where nothing else works. Likely the larger item of the three: the shell's canvas
  sizing, the player's fit, and every pointer and touch coordinate are involved.

## Queued 2026-10-02 (user, PaperKid)

- **Auto exposure settles visibly at the start of a scene** (user 2026-10-03): a scene loads with
  the exposure where it was (or the default) and adapts toward its target over a second or two,
  a noticeable dim and brighten every time a level starts. A new scene, or the first frame of a
  view, should start at its target exposure and adapt only from then on. Look at `ExposurePass`
  (its per-view history, `kMaxViews` slots indexed by view order, and what it starts from) and at
  whether a scene load resets it. PaperKid turned auto exposure off meanwhile; measured, its
  fixed EV 0 is 10 to 16% brighter than auto exposure settled at, so turning it back on will
  change the look a little.
- **TAA still looks jittery** (user 2026-10-03, "for later"): with the stale-history bug fixed the
  player matches the editor, but the resolve itself leaves visible jitter on edges. Look at the
  usual suspects in `TaaPass` and `taa` shader: the Halton sequence length (8) and jitter scale
  (computed from the target's size, not the viewport's: `PipelineImpl.cpp`, `HaltonJitter(...,
  v->Width(), v->Height())`), the YCoCg variance clip's gamma (1.25 in PaperKid) and blend factor
  (0.97), the depth-disocclusion reject, and whether history sampling (Catmull-Rom) or a
  sharpening pass is wanted. Measure frame-to-frame edge change on a static scene before and
  after each change (the method used for the stale-history bug).
- **`run::loadScene` misses scenes in a project-folder player**: run on a project folder,
  the player keeps authored scenes in its source database and hands DefaultApplication only
  the cooked one (`PlayerApplication.h`, `m_sceneDb` vs `SetContentDatabase(m_contentDb)`),
  so the script's `loadScene` / `loadSceneAsync` (and the prefab spawner and network prefab
  lookups that share that database) find no scene and return false without a log line.
  PaperKid's New game did nothing. A dist has one database, so exports are unaffected. Fix:
  give DefaultApplication the scene database separately, the player passing `m_sceneDb`, and
  log a failed load. Check what Sedulous's player does first.
- **The loading screen setting shows nothing** (found 2026-10-04): the project's `loadingDocumentId`
  ("the cooked UIDocument shown as the boot splash while the default scene loads",
  `Engine.Project/ProjectModule.cppm`) is saved, exported and listed by `project_info`, but no code
  in the engine or the player ever shows it. Build it, or drop the setting. When built: a loading
  screen belongs to the RUN that is loading, not the shared screen tier, or every editor Game tab
  shows it (Documentation/Specs/run-screens.md: each run has its own tier with run screens on;
  the overlay API, `PushScreenOverlay` / `RemoveScreenOverlay`, has no run yet and only the
  shared tier's overlay layer; give it a run as `ScreensFor` has, keeping truly global overlays,
  a debug badge, shared). Check what Sedulous does first.
- **pie_run cannot probe a behaviour's vector field** (found 2026-10-04, PaperKid's throws): a probe of
  `Bike.m_target` (a private `Float3` member of the Bike behaviour) reads null, and `Bike.m_target.x`
  answers "nothing at 'x'", while the behaviour's float fields probe fine. Probes should read a script
  value type (Float3, Quaternion, Color) and its components, as they do an entity's `worldPosition`.
- **Save data follow-ups** (2026-10-04, Documentation/Specs/save-data.md "Not now"): a web backend
  (the web build's user data directory is in memory, so a save lasts one page load: browser storage,
  IndexedDB); an MCP tool to read or clear the play-in-editor save, so an agent testing a "best
  score" can start from nothing without editing `Editor/` by hand; the player's audio volumes still
  written at shutdown only, not through the atomic write at the moment they change.
- **The editor crashes when the Console panel is squeezed very short** (found 2026-10-03, taking
  PaperKid's screenshots): with the dock layout's top area at 97% of the window height, the editor
  asserts on start in `LogView`'s list adapter (`LogView::Adapter::BindView`, `LogView.cppm:381`,
  `index < m_size`, from `ListView::OnLayout`): a row index past the filtered rows. At 90% and
  92.5% it starts. A list must not bind a row it does not have, however short the panel.
- **`var()` inside a drawable's arguments draws white, silently** (found 2026-10-03, PaperKid's
  theme): `background: rounded-rect(var(--paper), radius=6, border=var(--ink), ...)` in a project
  theme (`.sss`) draws a plain white rect with no border, and the log says nothing; `var()` works
  as a whole property value (`text-color: var(--ink)`). A project theme cannot declare a `$name`
  palette either (the palette is the engine theme's `ThemePalette`), so a theme writes every
  colour out. Fix: resolve `var()` in factory arguments at compute time (or a sheet-declared
  palette), and warn on an argument that does not parse. Check Sedulous first.
- **Every new particle system has the same random seed** (found 2026-10-03, PaperKid's confetti):
  `ParticleSystem`'s default seed is one constant, so two systems in an effect spawn their
  particles in identical places and only the last drawn shows (four confetti colours drew as one
  green). PaperKid's `fx.py` seeds each system by name. Fix: give a system a fresh seed when the
  particle page (and `asset_create`'s default) adds one.

## Queued 2026-10-02 (user, render-textures review)

- **Post effects for an orthographic camera**: `LimitPostForOrthographic`
  (`Engine.Render/RenderComponents.cppm`, applied in `RenderSubsystem::RenderScene`) turns off AO,
  SSR, SSGI and TAA for any orthographic view, because their shaders assume a perspective
  projection. A target camera (the minimap) renders post-off anyway; this matters for an
  orthographic MAIN camera (isometric, 2.5D). What each needs:
  - **AO (GTAO/SSAO)**: view-space position is rebuilt from depth with `ProjXX`/`ProjYY` and
    perspective depth; an orthographic branch (linear depth, x/y not scaled by distance). Small.
  - **TAA**: reprojection is matrix-based and fine; the history rejection linearizes depth as
    perspective (`LinearizeDepth` in `taa.ps.hlsl`). Small.
  - **SSR, SSGI**: screen-space marches assume rays fanning from an eye point; in orthographic
    every view ray is parallel, so the ray setup and the step into screen space change. Larger.
  Do AO and TAA first, then lift them out of `LimitPostForOrthographic`; each with a render
  test, checked in an orthographic scene by the user. Unity URP supports SSAO in orthographic;
  Unreal and Godot leave some screen-space effects limited there (from memory - check their
  current docs before citing).

## Queued 2026-09-23 (user, during the terrain holes work)

- **A blank flat heightfield cannot be dug into** (confirmed): a blank is zero-filled, sample 0
  = Min Height, and the lower brush (SculptRaise with a negative strength) clamps at sample 0;
  editing Min Height moves the whole plane. Build: a base height on blank creation (a fresh
  flat sits mid-range), or a "base height" field on the heightfield page that re-quantizes.

## Queued 2026-09-23 (the editor main-thread work)

- **Large source scene parse on the UI thread**: with the binds async, a scene page's remaining
  UI-thread cost is the XML parse of the source (Bistro prefab: 49 MB, 2.7 s of a 2.9 s open).
  Options: parse on a worker (page shows a placeholder until the scene lands) or a binary source
  form for machine-generated scenes. Measure with the page's `opened scene` line.
- **Texture finalize at ~100 ms** (Bistro open after the settle-reload fix, Debug + validation):
  44 of 816 finalizes past 50 ms, all `Texture`, tightly clustered at 90-105 ms - a uniform
  wait, not size-proportional (a transfer-batch submit + wait?). Not a freeze (the burst pops in
  over 20 s at the 2 ms pump budget); measure in RelWithDebInfo before chasing, per the user
  (Debug-only shader compile / pipeline creation is expected and not this).
- **Remove the vegetation legacy reader** (`ReadsDataVersionsFrom(1)` on
  `TerrainVegetationComponent`) once the user's scenes are re-saved at version 2.
- **Per-mesh import bookkeeping** (~0.5 s of Bistro's 0.9 s main-thread import): ClaimInstance +
  XML envelope writes per mesh; a batched claim if a bigger model shows it.

## Open follow-ups filed during week 2026-09-05 (moved 2026-09-12)

Small items left open inside that week's DONE/audit sections; the full write-ups are in
week-2026-09-05.md under the section named.

- **UI visual pass at the desk** (spec ui-layout-and-style-model.md, "Visual iteration"):
  the import dialog and the inspector are the yardsticks, both called "disliked"; a design
  session with the user, not engineering. The style model P0-P4 is closed.
- **UI style model, NOT built by choice** (spec STATUS blocks): P1 `[attr=value]` and
  `:selected`; P2 per-edge borders, per-corner radius, gradient backgrounds, `cursor` /
  `visibility` from sheets, composited (not per-vertex) opacity, percent insets on absolute
  children; P3 transition longhands, `cubic-bezier()`, `steps()`, transition events; P4
  `flex-shrink` (still unread), `flex`/`flex-flow` shorthands, `wrap-reverse`, `order`,
  `align-content: space-evenly`, ellipsis on wrapped text. Each waits for a consumer.
- **Beef UI review remainder** ("Beef UI line reviewed", week-2026-09-12): mirror the
  ContentButton (4) and DrawableView/Separator/ProgressBar unbounded-measure cases their
  port added; decide whether UndoStack's `i32&` outputs should be spelled as outputs and
  whether RemoveView's advisory `deleteChild` flag earns its place; the sandbox tree rows'
  hard-coded 20px indent (cosmetic).
- **Beef Engine-layer review**: the agent has started the Engine layer under the new
  copy-and-strike ledger; review its commits the same way (read the bodies for "For the
  Raptor agent" / "FOR RAPTOR REVIEW" / "worth mirroring").
- **Net, deferred with the unification ruling** ("Net: unification ruling"): IPv6 (the u64
  DatagramEndpoint packs an IPv4 quad) and TLS; the Win32 host-resolution path is written
  but the MSVC lane is the user's to run.
- **ScrollView axis asymmetry** (their ScrollView port note): vertical measures unbounded
  unless the policy is Never, horizontal only when Always. Judged the intended document
  shape; pin it with a test if it is ever revisited.

## Seeded: occlusion culling (user schedule 2026-08-26)

Origin: Documentation/Parity/LumixEngine/parity-2026-08-26.md - occlusion
culling is called out as ABSENT ON BOTH SIDES (the GPU-driven bullet + the
gap list). Draconic already culls by view frustum (brute-force bounding
sphere, default-off - [[view-frustum-culling]]) and by screen-coverage LOD,
but NOTHING skips geometry hidden BEHIND other geometry, so a dense interior
still pays for every occluded draw. `DrawIndirect` exists in the RHI and is
UNUSED - occlusion culling is the natural first consumer of a GPU-driven path.

Goal: skip (or GPU-cull) draws whose bounds are fully occluded, layered on top
of the existing frustum + coverage culls, per view.

Approach decision is the FIRST gate (design before build):
- **Hi-Z GPU occlusion (two-phase)**: we already run a depth prepass (MRT /
  depth-prepass, [[renderer-postfx-plan]]); build a Hi-Z depth pyramid from it
  and test each object's bounds against the coarsest covering mip, then a
  second pass for the false-negatives. Pairs with the unused `DrawIndirect` and
  is the modern GPU-driven shape - but WebGPU has no cheap GPU->CPU readback, so
  the cull result must stay on-GPU (indirect draw args), not stall the CPU.
- **CPU software occlusion**: rasterize a few chosen occluders into a tiny
  depth buffer and test bounds CPU-side. Portable (no backend divergence, works
  on the Null device + headless tests), no readback latency, but needs
  occluder selection and costs CPU.
- Portability-first is the house bias (Vulkan + WebGPU + DX12), so weigh the
  Hi-Z path's WebGPU story carefully; a CPU pre-pass may be the pragmatic v1
  with the GPU path as the follow-on that also lights up `DrawIndirect`.

Open questions for week start:
- Which path (Hi-Z GPU vs CPU software vs hybrid), and does v1 target meshes
  only (like the LOD v1) or also terrain chunks / instanced sets?
- Occluder selection: all opaque geometry, or a curated occluder set?
- Interaction with clustered-forward + the depth prepass already in the graph
  (occlusion wants to run AFTER prepass, BEFORE the opaque pass records draws).
- Test story: coverage math is unit-testable headless (bounds-vs-Hi-Z or
  bounds-vs-software-depth); on-screen = a boxes-behind-a-wall probe.

## Seeded: investigate deferred rendering (user 2026-08-26)

Origin: same parity doc - Draconic is clustered FORWARD (GPU-built froxels,
16px tiles, MSAA-compatible) while Lumix is DEFERRED (CPU-built 64px clusters,
cheaper at high light counts, no MSAA). Several Lumix-ahead items downstream of
having a G-buffer: deferred G-buffer decals (ours are v1 forward
fullscreen-tri-per-decal blends - [[decals]]), cheaper many-lights.

This is an INVESTIGATION, not a build - produce a written recommendation, not a
renderer rewrite. The question: should Draconic add a deferred (or hybrid /
visibility-buffer) lighting path, or stay clustered-forward?

Grounding the investigation must not skip:
- We ALREADY write a fat MRT in the forward path (SV_Target1 view-space normal,
  Target2 motion, Target3 roughness/metallic - the terrain PS + mesh PS write
  the full 4-target GBUFFER the post stack reads). So a deferred path is closer
  than "from scratch" - it moves lighting to a fullscreen pass over that buffer.
- What forward BUYS us that a naive deferred loses: MSAA (clustered-forward is
  MSAA-compatible; classic deferred is not), portability (the whole
  Vulkan/WebGPU/DX12 clustered-forward + froxel + post infrastructure is built
  and test-covered - [[renderer-roadmap-status]]), and per-material shading.
- What deferred BUYS: high-light-count cost, G-buffer decals, and a cleaner
  many-effects G-buffer surface - weigh against the above.
- Consider the MODERN alternatives, not just classic deferred: a hybrid (deferred
  opaque + forward transparents/MSAA-edge), or a visibility-buffer / deferred-
  texturing path that sidesteps the fat-G-buffer bandwidth. Note the dead
  mesh-shader + ray-tracing pipeline objects that exist unused (parity §1 LOD).

Deliverable: a decision doc (Documentation/Specs/ or a Parity follow-up) with a
recommendation - stay forward, add an OPTIONAL deferred path, or go hybrid - and
if "add", a phased sketch. No renderer code this week.

## Seeded: GPU profiler + profiler visualization (user 2026-08-26)

Origin: same parity doc - "GPU PROFILER: Lumix-ahead - unified CPU/GPU profiler
timeline + pipeline statistics + NVML telemetry + PIX/RenderDoc; Draconic:
per-pass graph timestamps + CPU record-time report, SEPARATE from its CPU
profiler." We have both halves but they are text-dump only and disjoint.

What exists ([[profiler-system]]): `draconic.profiler` CPU scopes
(compile-gated) + a GPU `GraphProfiler` (per-pass Vulkan timestamps); the P key
DUMPS TEXT. Two gaps, and the user asked for both:

1. **GPU profiler** - broaden past the current per-pass Vulkan-only timestamps:
   finer scopes and coverage on the OTHER backends (WebGPU timestamp-query where
   available, DX12) so the GPU timeline is not Vulkan-only. Pipeline statistics
   (draw/primitive counts) alongside the timings if cheap.
2. **Profiler visualization** - replace the text dump with a VISUAL profiler
   panel (timeline / flamegraph) that UNIFIES the CPU scopes and the GPU
   timestamps on ONE per-frame timeline (the "separate" problem the parity note
   calls out). ImGui is already integrated ([[imgui-integration]]) - a candidate
   host for the overlay - but decide editor-panel vs in-app-overlay at design.

Open questions for week start:
- The shared data model: merge CPU scope trees + GPU pass timings into one frame
  record with a common clock/axis (GPU timestamps resolve a frame or two late -
  the view must align them to the right CPU frame).
- Where it lives: an editor profiler panel, an ImGui overlay in the samples, or
  both reading the same capture. Keep the capture layer UI-free.
- Portability: WebGPU timestamp-query is optional/limited and DX12 differs from
  Vulkan - the GPU side degrades gracefully where timestamps are unavailable.
- Compile-gating stays (the CPU profiler is already gated); the visualization is
  a debug tool, not shipped in dist.

## Seeded: contact shadows (user 2026-08-26)

Origin: same parity doc - "Lumix-only: contact shadows (SSS)" (also in Lumix's
deferred-effects list). Draconic ships CSM (cascaded shadow maps, texel-snapped,
per-cascade caster culling - [[renderer-roadmap-status]], shadows track) but has
NO screen-space contact shadows, so fine contact occlusion the cascade
resolution misses (the gap right where objects touch a surface, thin-feature
peter-panning) is absent.

What they are: a SHORT-RANGE screen-space ray march along the light direction
through the DEPTH buffer, adding fine occlusion at contact points. COMPLEMENTS
CSM (min of the two shadow terms), does not replace it.

Why it fits cleanly: we already have a depth prepass + the directional light in
the terrain/mesh view UBO. A contact-shadow term reads depth + light dir,
marches a handful of steps in screen space, and attenuates the DIRECT sun term.
Portable - it needs only the depth buffer, so it works Vulkan / WebGPU / DX12
with no special feature (unlike the DX12-only Lumix effects).

Scope for the week: a directional-light (sun) contact-shadow term with the usual
knobs (step count, max world distance, thickness), combined with the CSM sample.

Open questions for week start:
- Where it slots: folded into the forward lighting sun sample (cheapest, reuses
  the depth already bound) vs a separate screen-space pass writing a shadow
  factor the lighting reads.
- Noise/undersampling: dither the march start and let TAA resolve it (we have TAA
  - [[renderer-postfx-plan]]); pick step count vs cost.
- Sun-only v1 vs also the strongest local lights (defer locals unless cheap).
- Tests: a probe where an object hovering just above a surface shows a contact
  shadow CSM alone misses; Vk/WebGPU parity; the no-op case (contact shadows off)
  stays byte-identical.

## Seeded: CDLOD morph for terrain seam quality (user 2026-08-26)

Origin: same parity doc, terrain section - "Verdict: Lumix-ahead on seam
quality, Draconic-ahead on culling; CDLOD morph is Draconic's documented
deferral." Our terrain HARD-SWITCHES chunk LODs and hides the resulting cracks
with double-sided skirts (Terrain.cppm:124-146), which is robust and portable
but POPS at the transition; Lumix geomorphs at ring boundaries for popping-free
seams. This closes that gap. It was explicitly deferred in terrain.md and the
[[terrain-track]] deferral list.

Goal: continuous (CDLOD-style) LOD - in the terrain VS, morph a chunk's
higher-detail vertices toward their lower-detail PARENT positions across a
transition band as the camera nears the LOD switch, so a chunk blends smoothly
into the next level instead of snapping. Popping-free seams, no skirt pop.

Why it fits our model (and stays portable): it is a VERTEX-SHADER change, not
tessellation - the shared 65x65 chunk grid morphs odd vertices toward their even
(parent-grid) neighbours by a per-vertex morph factor derived from camera
distance vs the chunk's LOD range; height re-sampled from the already-bound
height texture. No hull/domain shader, so the WebGPU-portability ruling
(terrain.md:165-167, one path incl. WebGPU) HOLDS.

Scope for the week: the morph factor (per chunk, from its screen-coverage LOD
distance band - align the morph window to the EXISTING coverage LOD selection +
hysteresis, [[view-frustum-culling]] / mesh-lod coverage math) + the VS vertex
morph + a decision on skirts (morph aligns edges, so skirts may shrink to a
safety net or drop). Terrain-only.

Open questions for week start:
- Morph-factor source: a per-chunk uniform LOD-range (start/end) the VS lerps
  over, vs fully per-vertex - keep it aligned with the coverage LOD + hysteresis
  that already picks the chunk LOD so the morph completes exactly at the switch.
- The DEPTH/shadow VS: morph there too (so shadows match) or keep the coarse
  clamp (shadow-pass LOD clamping already exists) - matching avoids self-shadow
  swim.
- Skirts: keep as a belt-and-suspenders crack guard, or remove once the morph
  aligns boundary edges - measure.
- Tests: the morph-factor math is unit-testable headless (distance -> factor,
  boundary continuity); a Vk/WebGPU terrain probe that the morphed boundary is
  seamless (band-edge vertices agree with the neighbour LOD); morph-off stays
  byte-identical to today.

## Seeded: erosion + hole cutting for terrain (user 2026-08-26)

Origin: same parity doc, terrain section - "Both lack erosion + hole cutting"
and (holes/caves) "Both absent". Both are documented Draconic deferrals
([[terrain-track]] deferral list: holes, erosion/derived bakes). We already have
strong terrain sculpt: headless-testable brush cores (raise/lower/smooth/flatten,
Heightfield.cppm), region-delta undo per stroke, live BumpVersion re-upload. Two
DISTINCT workstreams hang off that spine - pick phasing at week start.

### Erosion (a heightfield transform)

What: hydraulic + thermal erosion on the heightfield samples - carve drainage,
settle talus slopes, deposit sediment - the natural-terrain look sculpting alone
cannot cheaply make. It is a SAMPLE-MATH op on the pure Heightfield model (the
headless-testable layer), so it reuses the sculpt spine: region-delta (or
full-field) undo + BumpVersion re-upload; no render change.

Scope: a terrain-page/tool BAKE with parameters - thermal (talus angle) as the
cheap starter, then hydraulic (droplet or grid, iterations / rain / evaporation /
sediment capacity). Live as a whole-terrain bake button and/or an erosion brush.
Open Qs: whole-field vs brush; a reusable Heightfield::Erode* core the sculpt
tool and a page button both call; determinism (seeded) for the headless tests.

### Hole cutting (a new terrain channel + render + physics)

What: mask out terrain cells for caves / cellar mouths / tunnels - a per-texel
HOLE MASK; the renderer skips holed cells and physics skips collision there.

The pieces:
- **Data**: a dedicated hole raster on the terrain (1 bit/texel), cooked to a
  sidecar exactly like the splat weights / palette streams (and DELETED-when-empty
  per the on-demand-sidecar rule just fixed). A hole BRUSH paints it on/off on
  the same brush spine.
- **Render**: v1 = sample the hole mask in the terrain PS and `discard` holed
  cells (portable, alpha-test-style, works Vk/WebGPU/DX12); crisp index-level cell
  skip at chunk build is the follow-on. Watch the interaction with skirts + the
  CDLOD morph at hole edges.
- **Physics**: Jolt HeightFieldShape already supports non-colliding cells
  (`cNoCollisionValue` - the collider path pads with it today, [[terrain-track]]);
  holed samples map to that at shape build, so collision opens up for free through
  the existing shared-Ref<Heightfield> collider. Verify the mapping.

Open Qs: hole-mask storage (dedicated 1-bit raster vs a sentinel height like some
engines use) - a dedicated mask is cleaner and avoids height aliasing; render
approach (PS discard v1 vs index skip); edge cleanup where a hole meets a skirt
or a morph band.

Tests: erosion core is deterministic + headless (seeded field -> expected
carve/deposit); hole mask round-trips through cook (present/absent + delete);
a Vk/WebGPU probe that a holed cell renders nothing and a Jolt test that a ray
passes through a holed cell but hits an unholed one.

## Seeded: grass / vegetation / foliage for terrain (user 2026-08-26)

Origin: same parity doc - "GRASS/VEGETATION: LUMIX AHEAD - TOTAL GAP, the LARGEST
in this domain. Draconic: zero vegetation of any kind ... planned as terrain
phase 3." Also the in-session user question (2026-08-26): how to paint grass on
terrain - answered as a SEPARATE scatter system, procedural density-grass first,
prop scatter second. This is a big track: DESIGN FIRST (write the spec), then
phase the build.

What Lumix ships (the target to reach, not copy verbatim): density-mask-driven
procedural grass (a type mask in the splatmap, ~1024 candidates/quad), per-quad
instance buffers + AABB cull + distance density fade, an authoring brush, prefab
scattering, and baked octahedral impostors as the far LOD.

Our hooks (why this is buildable on what exists):
- **Render path**: `InstancedMeshComponent` is SHIPPED (persistent buffer, shared
  ramp, O(1)/frame - [[instanced-mesh]]); grass renders as instanced draws, no
  new render primitive.
- **Partitioning**: the terrain quadtree already gives per-chunk sets - the
  [[terrain-track]] note is that grass instanced sets partition PER CHUNK so the
  shipped per-set coverage LOD selection + hysteresis works FOR FREE (build/free
  a chunk's grass set as it LODs / comes into range).
- **Placement mask**: a density/type mask (a dedicated grass raster, or derived
  from the top-K splat) drives where + which grass scatters.

Phasing (pick how far at week start; likely spec + P0 only):
- **Spec first** - the queued foliage design (procedural grass, then prop
  scatter). Land the mask + scatter + LOD/cull model on paper before code.
- **P0 procedural grass** - per-chunk deterministic (seeded) scatter of grass
  instances from the density mask into instanced-mesh sets, distance density
  fade + per-chunk AABB cull, optional VS wind sway. CPU scatter -> instanced
  draw is the portable v1; GPU-driven scatter (the compute cull/LOD/indirect
  chain) is the follow-on and shares the occlusion / DrawIndirect work seeded
  this same week.
- **P1 prop / prefab SCATTER brush** - paint mesh instances (rocks, bushes) with
  density + jitter + slope/height rules + collision rejection.
- **P2 DEFER** - baked impostors as the far LOD (also a standalone parity gap),
  GPU-driven scatter culling.

Open questions for week start:
- Density/type mask: a dedicated grass raster (paint brush + cooked sidecar, the
  splat-weights pattern) vs deriving from the splat top-K - a dedicated mask is
  more controllable but is another channel.
- Determinism: seed scatter PER CHUNK so it is stable frame-to-frame AND
  headless-testable (seed -> expected instance transforms).
- Set lifecycle: build/free per-chunk instanced sets as chunks stream + change
  LOD; interaction with the CDLOD morph seeded this week (grass rides the morphed
  surface height).
- Wind: VS sway now vs later; culling reuses the terrain quadtree AABBs.

Tests: deterministic scatter (seed -> instance transforms, headless); per-chunk
set build/free; a Vk/WebGPU probe that grass renders + density fades with
distance; scatter-off byte-identical.

## Seeded: root motion for animation (user 2026-08-26)

Origin: same parity doc, animation section - "ROOT MOTION: Lumix decisive - full
pipeline ... Draconic: ZERO hits for RootMotion/rootMotion across Code/. HARD
BLOCKER for locomotion-driven characters." We have the surrounding stack
(skeletal runtime, a Mecanim-style AnimationGraph FSM + BlendTree2D, clip/graph/
skeleton editor pages, CharacterVirtual) but nothing extracts or applies the
root bone's motion, so animation cannot DRIVE character movement. Directly
relevant to PaperKid ([[first-game-paperboy]]) if the bike/character is
animation-driven. Cross-cutting (import + clip resource + graph + scene +
physics) - DESIGN FIRST, then phase.

The four pieces (mirror Lumix's shape on our stack):
1. **Author/flag** - a per-clip root-motion flag + which bone is the root;
   import-time identification of the root track.
2. **Bake-time strip** - extract the root bone's per-frame DELTA (typically
   planar XZ translation + yaw) OUT of the pose so the skeleton animates
   IN PLACE, storing the delta separately on the clip resource
   (AnimationResource). New sidecar/field + a version bump (serializer-strict).
3. **Graph accumulation** - the AnimationGraph blends the per-clip root deltas
   by the SAME weights it blends poses (per-node accumulation), producing one
   root delta per frame. This is the subtle correctness piece.
4. **Scene application** - a component applies the accumulated delta to the
   entity Transform each frame; for a physics character, feed it to
   CharacterVirtual ([[physics-p1]]) instead of teleporting the transform.

Scope for the week: likely the spec + P0 (flag + bake strip + single-clip apply
to the entity transform) so a walk clip MOVES the entity in place; graph
blending of deltas + the CharacterVirtual path as P1.

Open questions for week start:
- Root DOF: planar XZ + yaw only (the common locomotion case) vs full 6-DOF -
  start planar, keep the data general.
- Loop wrap: the delta across a clip's loop boundary must be continuous (no
  teleport at wrap).
- Blend correctness: blending DELTAS by pose weights (and across FSM
  transitions / exit-time) must not drift vs the visual pose blend.
- Physics: apply to CharacterVirtual (collide + slide) vs a kinematic transform
  for non-physics actors - support both.
- Editor: the clip/graph preview shows in-place vs moving; the clip page marks
  root motion.

Tests: bake extraction (moving-root clip -> in-place pose + expected per-frame
delta, headless); two-clip delta blend by weight; application accumulates to the
right end transform over N frames; loop-wrap continuity; root-motion-off is
byte-identical (clip plays as authored).

## Seeded: inverse kinematics (user 2026-08-26)

Origin: same parity doc, animation section - "IK: Lumix decisive - FABRIK N-bone
graph node, editor-authorable. Draconic: none (only camera LookAt matrices).
Neither has foot-IK / look-at / two-bone." A total gap; sibling to root motion
(both are POST-graph pose modifications, and foot-IK plants feet DURING
locomotion so it pairs with the root-motion work above).

Goal: solve bone chains to reach targets - a pose-modification pass that runs
AFTER the AnimationGraph produces the pose and BEFORE skinning, adjusting named
chains toward world targets (hand-to-object, foot-to-ground, head look-at).

Solver set (build cheapest-first):
- **Two-bone (analytic)** - arms/legs; exact, cheap; the workhorse for foot +
  hand IK.
- **Look-at / aim** - single-bone (head/eyes/weapon) toward a target.
- **FABRIK (N-bone iterative)** - general chains (spines, tails); Lumix's choice.
- **Foot-IK** - two-bone legs + a ground RAYCAST (physics is available,
  [[physics-p1]]) + pelvis drop, so feet plant on uneven ground.

Where it runs (design decision at week start): AnimationGraph IK NODES (Lumix's
shape - authored in the graph) vs a post-graph list of IK CONSTRAINTS on the
animator component. Either way it mutates the local/model pose on the Skeleton
before the palette is computed, with a per-constraint blend alpha (IK weight).

Scope for the week: likely spec + P0 = the two-bone solver + look-at, applied
through one chosen seam with a blend weight; FABRIK + foot-IK (ground ray +
pelvis) as P1; editor chain authoring as it matures.

Open questions for week start:
- Target source: a world transform, another entity, or a raycast hit (foot).
- Pole/hint vectors for the two-bone knee/elbow direction.
- Blend: per-constraint alpha (IK off <-> full) so IK eases in/out, not a hard
  snap; ordering when multiple constraints touch overlapping bones.
- Interaction with root motion: foot planting vs the locomotion delta.
- Editor: how chains are authored (graph node vs component) + previewed.

Tests: two-bone reaches a REACHABLE target exactly + clamps an unreachable one
to max extension (headless); look-at aims within tolerance; FABRIK converges;
blend alpha 0 is byte-identical to the un-modified pose.

## Seeded: ragdoll (user 2026-08-26)

Origin: parity doc - "Both absent: ragdoll (Lumix REMOVED theirs; Draconic has
Jolt's Ragdoll / SkeletonMapper VENDORED but UNWIRED)." So unlike most gaps this
is a WIRING job, not a port: `ThirdParty/JoltPhysics/Jolt/Physics/Ragdoll/`
(Ragdoll.cpp/.h) + Jolt's SkeletonMapper already ship; nothing above the physics
layer drives them. We have the skeleton + Jolt world + CharacterVirtual
([[physics-p1]]) + the animation pose to feed it.

Goal: physics-driven skeletons - bones become constrained rigid bodies for
death/impact reactions, and (powered/active ragdoll) blend between the animated
pose and the simulation.

The pieces:
- **Ragdoll asset** - per-bone collision shapes + joint constraints (limits)
  built FROM the skeleton, authored/generated (Jolt's RagdollSettings shape).
- **Activate** - switch a character from animated to simulated: instantiate the
  Jolt Ragdoll, seed body poses from the current animated pose (no snap).
- **Read-back** - map the simulated body transforms back onto the skeleton pose
  (Jolt SkeletonMapper does exactly this) so skinning follows the simulation.
- **Blend (follow-on)** - powered ragdoll / partial (upper-body hit reaction) /
  get-up, driving Jolt motors toward the animated pose.

Scope for the week: likely spec + P0 = a ragdoll asset from the skeleton +
full-body activate + SkeletonMapper read-back (a character that goes limp on a
trigger). Powered/partial/get-up as P1.

Open questions for week start: where the ragdoll asset lives + how it is authored
(from the skeleton page? auto-generated shapes?); the animator <-> ragdoll
hand-off (pose seeding + read-back each frame); interaction with CharacterVirtual
(disable the controller while simulated); LOD/sleep for many ragdolls.

Tests: asset build from a skeleton (bodies + joints match the bone hierarchy);
activate seeds bodies from the pose (no first-frame snap); SkeletonMapper
read-back drives the pose (headless, a dropped ragdoll settles); ragdoll-off is
byte-identical (animation plays normally).

## Seeded: morph target animations (user 2026-08-26)

Origin: parity doc - "Both absent: ... morph targets (Draconic has a RESERVED
SEAM AnimationPose.cppm:23 + importer TODO; Lumix ZERO TRACE)." A chance to get
AHEAD, not just reach parity: the runtime pose already reserves
`morphWeights` (per-morph-target weights, `HasMorphWeights()`), and glTF carries
morph targets natively - so the seam exists, the import path just does not read
them yet.

Goal: blend-shape / morph-target animation - per-vertex position (+ optional
normal) DELTAS blended by weight, for facial expressions, shape variation, and
authored deform animation.

The pipeline (mirrors the skin/LOD stream shape we already cook):
- **Import** - the glTF loader ([[image-model-port]]) reads the primitive morph
  targets (position/normal deltas per target) + default weights + any animation
  channels that target the weights array (the importer TODO).
- **Cook/store** - the cooked mesh gains a MORPH-TARGET stream (N targets of
  vertex deltas) beside the geometry/skin streams; version bump = the migration
  (pre-bump meshes = zero targets).
- **Drive weights** - per-target weights over time. Our property/timeline
  animation is decisive (parity) - weights can ride a weight track, or a
  dedicated morph channel on the clip; the reserved `AnimationPose.morphWeights`
  carries them into the frame.
- **Apply** - the skinning VS sums `pos += sum(w_i * delta_i)` BEFORE the bone
  palette (a small loop for facial rigs; a sparse/compute path if target counts
  grow). Portable - delta buffers bound + a weights uniform, no special feature.

Scope for the week: likely spec + P0 = import + cook the delta stream + VS apply
with SET/scripted weights (a face mesh deforms when a weight changes); animating
the weights via clips/property animation as P1.

Open questions for week start:
- Weight-count budget + the VS-loop-vs-compute threshold (facial rigs are few
  targets; some assets have dozens).
- Deltas: positions only vs positions + normals (lighting on strong morphs).
- Where weights are authored/driven: reuse the property-animation channels vs a
  dedicated morph track on the clip.
- Interaction with skinning (morph THEN skin) + motion vectors (prev-frame
  weights, mirroring the prev-frame bone palette we already keep).

Tests: import round-trips N targets + deltas; VS apply moves vertices by the
weighted sum (a pixel/vertex probe: weight 0 == base mesh byte-identical, weight
1 == the target); weights animate over a clip; compat (a no-morph mesh loads as
zero targets, byte-identical).

## Seeded: animation compression (user 2026-08-26)

Origin: parity doc - "Compression: Lumix decisive - constant-track collapse +
per-channel bit-packing driven by authored error tolerances + smallest-three
quats, with in-editor size stats. Draconic stores RAW f32 keys
(AnimationResource.cppm:163) - THOUGH variable keyframe density + cubic tangents
are preserved (the better substrate for a future compressor)." So we pay full
raw-key memory/bandwidth, but our data model is the RIGHT substrate to compress.

Goal: an offline (cook-time) clip compressor with error-bounded quality, a
compact runtime form the sampler decodes, and editor size stats.

The techniques (biggest-win-first):
- **Constant-track collapse** - a track whose keys never change (within epsilon)
  collapses to ONE value; huge for the many bones a clip does not move.
- **Keyframe reduction** - drop keys the curve can reconstruct within an AUTHORED
  per-channel error tolerance, exploiting the cubic tangents we already keep
  (fit-and-cull, not fixed decimation).
- **Quantization / bit-packing** - rotations as SMALLEST-THREE quats (drop the
  largest component, N-bit each + index); translations/scales range-packed per
  track to the authored precision.
- **Editor size stats** - per-clip before/after on the clip page (Lumix's parity
  feature).

Scope for the week: P0 = constant-track collapse + error-bounded keyframe
reduction (the memory/quality wins, minimal format churn); P1 = quantization /
bit-packing + the smallest-three quat form + the editor stats. Clip resource
version bump = the migration (raw clips still load; re-cook compresses).

Open questions for week start:
- Error tolerance: a global default + per-clip (or per-channel) override; the
  metric is POSE error (bone position/rotation deviation), not raw key delta, so
  a distant bone tolerates more angular error.
- Runtime cost: the sampler must decode the compact form cheaply (the sample hot
  path); keep the smallest-three unpack + range-expand branch-light.
- Ordering vs mesh LOD's meshopt work: independent, but both are cook-time size
  wins worth landing behind a size-report so regressions are visible.

Tests: a compressed clip samples within the authored tolerance of the raw clip
at every frame (per-bone pose-error bound, headless); a constant track collapses
to one key; measured size reduction logged by the cook test; sampler round-trip
(compress -> decode == within tolerance).

## Seeded: alpha-coverage-preserving mips + stochastic mips (user 2026-08-26)

Origin: parity doc, textures - "Lumix-ahead on two mip features Draconic lacks:
alpha-coverage-preserving mips and stochastic mips" (also the gaps list,
"coverage-preserving mips"). Both are ALPHA-TESTED (cutout) texture-quality
features - foliage cards, chain-link, grass blades - so they PAIR DIRECTLY with
the grass/vegetation item seeded this week (cutout grass thins and shimmers at
distance without them).

- **Alpha-coverage-preserving mips (COOK)**: a plain box-filter mip DOWNSAMPLE
  shrinks the fraction of texels above the alpha-test threshold, so a cutout
  silhouette THINS or vanishes at distance. Fix (Castano/NVIDIA): after each mip
  reduction, binary-search an alpha SCALE so that mip's coverage (fraction >=
  threshold) matches mip 0, and bake the scaled alpha. Lives in the texture cook
  (TextureAsset mip generation) behind a per-texture flag (auto for cutout/
  foliage presets). This is the general cousin of the terrain
  [[terrain-coverage-mask]] R4 deferral (coverage-preserving mip scaling).
- **Stochastic / hashed alpha ("stochastic mips") (SHADER)**: alpha-test with a
  per-pixel HASHED threshold so the cutout DISSOLVES smoothly (order-independent)
  instead of a hard binary edge, and mip transitions stop shimmering; TAA
  ([[renderer-postfx-plan]]) resolves the dither to smooth coverage. A per-
  material option on alpha-tested surfaces. (Confirm the exact reading of
  "stochastic mips" at week start - hashed-alpha testing vs stochastic mip-level
  selection; hashed alpha is the usual foliage companion to coverage mips.)

Scope for the week: P0 = alpha-coverage-preserving mips in the texture cook (the
concrete, testable win) + a cutout/foliage preset that turns it on; hashed-alpha
in the cutout material shader as P1, landing WITH or just before grass so its
cards read correctly at range.

Open questions for week start:
- Which textures opt in (a flag, auto-on for the cutout/foliage preset) and where
  the alpha-test threshold comes from (texture setting vs material).
- Cook version bump = re-cook affected textures; unaffected textures byte-
  identical.
- Stochastic reading (above) + TAA interaction (jitter the hash per frame so TAA
  integrates it).

Tests: coverage-preserving - a checkerboard/known-coverage alpha texture keeps
~constant coverage (fraction >= threshold) across ALL mips vs the box-filter
baseline that decays (headless, measured per mip); non-cutout textures byte-
identical; hashed alpha - the dither is deterministic + TAA-stable (probe).

## Seeded: impostors - octahedral bake as final LOD (user 2026-08-26)

Origin: parity doc - "Lumix ships octahedral IMPOSTORS (bake + runtime + shadow)
... impostor baking as the last LOD"; Draconic's mesh-LOD chain ends at the
COARSEST REAL MESH (no impostor level). Also the deferred P2 of the grass item
seeded this week. An impostor is a single billboard that samples a baked
octahedral atlas of a mesh's appearance from many view directions - it REPLACES
geometry entirely at extreme distance (forests, rock fields, distant props), the
biggest far-field win for dense vegetation.

Goal: an octahedral impostor as the LOD level AFTER the last real mesh LOD -
bake, runtime billboard, and a cheap shadow.

The pieces (builds on shipped mesh LOD [[mesh-lod-track]] + instanced mesh
[[instanced-mesh]]):
- **Bake (offline/editor)** - render N x N octahedral views of the mesh into an
  atlas: albedo + packed NORMAL (so it still lights) + DEPTH (parallax / edge).
  An editor bake step (the reflection-probe bake is the precedent for an
  editor-driven capture) producing an impostor asset / a mesh-LOD extension.
- **Runtime** - a camera-facing billboard, selected by the SAME coverage LOD math
  beyond the last mesh LOD; the VS/PS pick the octahedral cell by view direction
  (optionally blend the nearest cells to hide the pop), lit via the baked normal.
- **Shadow** - the impostor casts a cheap shadow (Lumix bakes this too).

Scope for the week: likely spec + P0 = bake + runtime billboard for ONE mesh at
extreme distance (a tree that becomes a card far away, view-correct). Cell
blending, shadow, and instanced-crowd integration (grass/forest) as follow-ons.

Open questions for week start:
- Atlas resolution + view count (octahedral grid density, e.g. 8x8-16x16);
  hemispherical (ground props) vs full sphere.
- Channels: albedo + normal + depth (depth for parallax/soft edge) - budget vs
  quality.
- Integration point: a SYNTHETIC final level in the mesh LOD chain (one
  Ref<StaticMesh> stays, coverage picks the impostor) vs a separate component.
- Adjacent-cell blend to hide the octahedral pop; where the bake runs + asset
  storage; the shadow-caster form.

Tests: bake produces a view-indexed atlas; the runtime billboard picks the
correct cell for a given view direction (the impostor from angle X matches the
real mesh's silhouette within tolerance, headless/probe); coverage LOD selects
the impostor beyond the last mesh LOD; impostor-off = the mesh chain unchanged.

## Seeded: PLAN - in-editor shader editing + material/shader node-graph editor (user 2026-08-26)

Origin: parity doc - "MATERIALS: par - both shader-ref + property-set with editor
pages, NO node graph either side" and "SHADERS: Draconic-ahead ... Lumix-ahead on
one nicety: in-editor shader editor WITH LIVE DISASSEMBLY." User asks for this as
a PLANNING item - the deliverable this week is a DESIGN DOC, not a build.

We are well-positioned (the plan should exploit what already exists):
- Offline shader cook to a versioned SPIR-V/DXIL/WGSL pack with a variant lint
  ([[shaders-track]]) - so "live disassembly" is our COOK OUTPUT, already produced.
- `CodeEditView` + per-language lexer registry ([[code-editor-track]]) - a shader
  code page reuses it (an HLSL lexer + save -> re-cook / hot-reload).
- `MaterialEditorPage` (shader-ref + property set + preview) - the material page a
  graph would feed.
- A `NodeGraph` UI foundation (post-port, [[nodegraph-evolution]]) + the working
  `AnimationGraphPage` node editor (3-pane, persisted node positions) as the
  precedent for a graph editor.
- Custom materials without renderer changes ([[renderer-direction]]) - what a
  shader graph would author.

Two candidate tiers for the plan to weigh + sequence:
1. **In-editor shader source editing (the cheaper win)** - open a `.hlsl` in a
   CodeEditView page, edit with the HLSL lexer, save -> the offline cook re-runs
   + hot-reloads, and show the cooked SPIR-V/WGSL DISASSEMBLY (Lumix's nicety, but
   from our compiler-free cooked pack, not a runtime FXC). Modest - reuses the
   code editor + the shader cook.
2. **Material / shader NODE GRAPH (the big feature; neither engine has it)** -
   visual nodes (texture sample, math, blend, ... -> surface output) that GENERATE
   HLSL fed through the EXISTING cook (SPIR-V/DXIL/WGSL, variant lint, compiler-
   free dist all still apply). Authors custom materials without hand-writing
   shaders. Rides the NodeGraph UI + the AnimationGraphPage editor pattern.

The plan must decide: ship tier 1 first (fast, high-value, low-risk) with tier 2
as the larger follow-on, or commit to the node graph directly; the node-graph
DATA MODEL (nodes -> generated HLSL, variant/feature interaction, the material
property surface it exposes); how generated shaders ride the variant/lint cook;
and the graph's serialization + editor shape (reuse AnimationGraphPage's spine).

Deliverable: the design doc (Documentation/Specs/) with the tiering
recommendation + a phased sketch for the chosen path. No shader-graph code this
week (tier 1's code page could be a stretch build if the plan lands early).

## Seeded: investigate large world support (user 2026-08-26)

Origin: parity doc - "Lumix-ahead on WORLD SCALE - DVec3 double-precision
positions + camera-relative getRelativeMatrix. Draconic is FLOAT-ONLY: no
large-world support." Our scene transforms store `Float3` positions, so beyond a
few km from origin f32 precision degrades - vertex swim, z-fighting, physics
jitter. INVESTIGATE first (does our content need it, and which approach), not a
committed build.

The approaches, cheapest-first (the investigation should tier + recommend):
- **Camera-relative rendering (the pragmatic first step)**: keep float storage,
  but translate geometry to be RELATIVE TO THE CAMERA before the GPU (subtract the
  camera position at high precision on the CPU, upload camera-relative matrices).
  Fixes the VISIBLE jitter/z-fighting with no storage change, and is PORTABLE -
  GPUs are float-only, so rendering MUST go camera-relative even if positions
  become double later. Likely the recommended v1; moderate, high-value.
- **Double-precision scene positions (Lumix's DVec3)**: store positions as f64,
  derive camera-relative float matrices for rendering. Fixes gameplay/physics math
  far out too, but INVASIVE (the whole transform system + serialization + physics
  interop; Jolt is float unless the heavy double build is used). Only if truly
  planet-scale.
- **Floating origin / rebasing**: periodically recenter the world around the
  player (shift all positions) to stay near origin - simpler than double, but
  needs discontinuity handling + touches anything caching world positions.
- Relates to the STREAMING/PAGING gap (parity: both absent; Draconic documents
  multi-terrain tiling as the interim) - tiles with local origins compose with any
  of the above.

Deliverable: a recommendation - do our target games need large worlds, and if so
which tier (almost certainly camera-relative rendering FIRST since it is the
portable prerequisite, double/floating-origin only if the scale demands it) - with
a phased sketch. No large-world code this week beyond a possible camera-relative
spike if the investigation lands early.

Open questions for week start: the target world SCALE (what do the games actually
need - a big level, or planet-scale?); how camera-relative threads through the
render graph + the froxel/clustered-forward view UBOs + shadows (all consume view
matrices); physics far-from-origin behaviour (Jolt float limits); whether
multi-terrain tiling already covers the near-term need.

## Seeded: gizmo vertex snapping (user 2026-08-26)

Origin: parity doc, editor - Lumix has VERTEX SNAPPING; "Draconic snaps drag
DELTAS only." Our transform gizmo quantizes the drag delta to grid increments
(translateSnap/rotateSnap/scaleSnap, Ctrl-held, PlayCanvas-style - Gizmo.cppm:
89-92), but cannot snap an object's point ONTO a vertex of other geometry -
precise assembly (align this corner to that wall's corner) is manual.

Goal: a vertex-snap mode for the translate gizmo - while dragging (a modifier /
toggle), snap the moved object so its snap SOURCE point lands exactly on the
nearest VERTEX of the geometry under the cursor.

The pieces (builds on the existing gizmo + scene pick):
- **Target pick** - ray-pick a mesh under the cursor (QueryRay, already used for
  selection/terrain), then find its NEAREST VERTEX to the hit (needs the mesh's
  vertices in world space at edit time - from the bound mesh resource).
- **Source point** - v1 = the moved object's PIVOT snaps to the target vertex;
  v2 = pick a source vertex ON the moved object (Blender-style) so any corner can
  be the anchor.
- **Apply** - offset the object so source -> target; this is an ABSOLUTE snap
  (unlike today's relative delta quantization), a distinct gizmo mode.

Scope for the week: P0 = pivot-to-nearest-vertex snap on the translate gizmo
under a modifier, with a highlight of the target vertex. Source-vertex pick +
edge/face (surface) snapping as P1.

Open questions for week start:
- UX: the modifier/toggle (a held key like Blender's V, vs a toolbar snap-mode
  toggle vs Ctrl-variant) - and coexistence with the existing delta snap.
- Vertex data access at edit time: nearest vertex to the ray hit - do we walk the
  bound mesh's CPU vertices, or sample the picked triangle's corners (cheaper, and
  enough for pivot->vertex)?
- Snap targets: vertex only (v1) vs also edges / faces / the grid; visual feedback
  for the candidate target.

Tests: with vertex snap on, dragging an object onto a mesh places its pivot at the
EXACT nearest vertex position (headless gizmo test, known mesh + known drag);
snap-off leaves the existing delta-snap behaviour byte-identical.

## Seeded: memory profiling (user 2026-08-26)

Origin: parity doc - Lumix has "a hierarchical MEMORY profiler with tag
allocators ... memory profiler UI grouping live allocations by stack tree";
"Draconic: no memory profiling" (called out as part of "Draconic's WIDEST editor
gap" with the CPU/GPU timeline). We already have the PRIMITIVE:
`TrackingAllocator` reports LiveBytes / TotalBytesAllocated / PeakBytes
(Core/Memory/TrackingAllocator.cppm) - what is missing is per-subsystem
attribution and a VIEW. Pairs with the GPU-profiler + profiler-visualization item
seeded this week (memory is the third panel beside CPU + GPU).

Goal: know WHERE memory goes (per subsystem/tag: render, physics, audio, scene,
UI, ...) with live + peak + an over-frame graph, and catch growth/leaks.

The pieces (build on TrackingAllocator):
- **Tagged allocators** - a per-subsystem tag/category so live/peak ROLL UP by
  owner (Lumix's tag allocators). Cleanest on our stack: a `TrackingAllocator`
  instance per subsystem (or a tag threaded through Allocate), reported into a
  registry.
- **Aggregation** - a registry that each frame collects per-tag live/total/peak
  (the numbers TrackingAllocator already exposes).
- **View** - a memory panel: grouped bars/tree by tag + a memory-over-TIME graph,
  UNIFIED into the profiler visualization (same panel as CPU/GPU). Compile-gated
  debug tool, not shipped in dist.
- **Stretch** - call-site / stack-tree grouping for leak hunting (ASAN already
  covers hard leaks; this is for growth attribution).

Scope for the week: P0 = per-subsystem tagged tracking + the aggregation registry
+ a live/peak readout and over-frame graph (reuse TrackingAllocator per
subsystem); stack-tree/call-site grouping deferred. Land the panel INTO the
profiler-viz work so there is one profiler surface, not three.

Open questions for week start:
- Tag assignment: a TrackingAllocator per subsystem (natural given the type
  exists) vs a tag argument on Allocate (finer but more invasive) - start
  per-allocator.
- Overhead: keep tracking cheap + compile-gated (the CPU profiler is already
  gated); atomics on the hot alloc path only when enabled.
- UI home: fold into the profiler-visualization panel (shared timeline) rather
  than a separate window.

Tests: a tagged-allocator registry rolls up per-tag live/peak correctly (allocate
+ free under tags, assert the rollup, headless); peak tracks the high-water mark;
tracking-off (gated out) is zero-overhead.

Context: the data source shipped with I5 this week (per-tag live/peak/total +
`MemoryTagReport()` rollup rows - record in week-2026-08-29.md); the
panel/graph work here remains.

## Seeded: editor key rebinding (user 2026-08-26) - BUILT 2026-09-27

Landed as layer 6 of Documentation/Specs/editor-actions.md (branch editor-actions-palette):
the named-action registry was layers 1 to 5, the per-user EditorShortcutSettings section,
the Preferences > Shortcuts page with key capture and collision detection (a collision keeps
the holder's chord and is reported by name; the policy chosen: warn and keep), and the
command palette beside it. The seed below is kept as the record of the ask.

Origin: parity doc, editor platform - "Lumix-ahead DECISIVELY on keybindings - a
full rebinding UI with COLLISION DETECTION vs Draconic's four HARDCODED
accelerators (Ctrl+Z/Y/S/...)". Our editor shortcuts are hand-registered on the
UI `ShortcutManager` (ApplicationImpl.cpp:2989+, `AddGlobal(KeyCode, Modifiers,
...)`) - fixed, not user-remappable, not persisted. NOTE: distinct from the GAME
input subsystem's rebinding ([[input-subsystem]], runtime rebind capture) - that
maps GAME actions; this is the EDITOR's own hotkeys.

Goal: user-rebindable editor shortcuts - a UI to view/change bindings with
collision detection, persisted per-user.

The pieces (build on the ShortcutManager + editor settings):
- **Named action registry** - each editor shortcut becomes a NAMED command
  ({id, default binding, handler}) instead of a hardcoded `AddGlobal` - so a
  binding can be remapped by action id. Migrate the existing Undo/Redo/Save
  accelerators into it first.
- **Rebinding UI** - a Preferences panel (the EditorPreferencesDialog exists)
  listing actions + their current binding, capturing a new key, and DETECTING
  COLLISIONS (two actions on one chord). The game input subsystem already has a
  key-CAPTURE widget/flow to reuse.
- **Persistence** - store overrides in the PER-USER editor settings
  (editor.settings.xml, the [[reorg-role-grouping]]-era EditorUiSettings home) as
  a new `EditorShortcutSettings` section; on startup the ShortcutManager registers
  each action's binding (default, unless overridden).

Scope for the week: P0 = the named-action registry (wrapping today's hardcoded
shortcuts) + persisted overrides + a rebind UI with collision detection. Per-user
(a shortcut is a user preference, not per-project - the asset-browser/scene-grid
scope precedent: match the scope of the thing).

Open questions for week start:
- Registry shape + how existing hardcoded `AddGlobal` sites migrate onto it.
- Collision policy: warn, block, or reassign-and-clear-the-other.
- Reuse the input subsystem's key-capture widget vs a small editor-local one.
- The per-user settings section + defaults (a missing/older file = built-in
  bindings; serializer-strict-gated).

Tests: the action registry round-trips a rebinding through the settings store
(default vs override); collision detection flags a duplicate chord; applying a
stored override registers the new chord on the ShortcutManager; no override =
the built-in accelerators unchanged.

## Seeded: crash reporting + dumps (user 2026-08-26)

Origin: parity doc, editor platform - Lumix has "crash reporting (WINDOWS)";
Draconic has none. A chance to be AHEAD via portability: Lumix's is Windows-only,
ours should be CROSS-PLATFORM (Linux + Windows; web is separate) - the house bias
is platform logic in Core/System BACKENDS, not #if ([[naming-full-and-backend-fanout]]).

Goal: on an unhandled crash (segfault / abort / unhandled exception), capture a
STACK TRACE + a minimal dump + the log tail to a crash folder, so a crash in the
editor or the exported PLAYER leaves a diagnosable artifact instead of vanishing.

The pieces (a Core/System platform feature, per-backend):
- **Handler install** - POSIX: `SIGSEGV`/`SIGABRT`/`SIGFPE` handlers; Win32:
  `SetUnhandledExceptionFilter`. Async-signal-SAFE on the POSIX path (minimal
  work, no allocation in the handler).
- **Capture** - a backtrace (POSIX `backtrace` / Win32 StackWalk) + the crashing
  context; include build id/version, loaded modules, and the recent LOG TAIL (the
  logger already buffers).
- **Write** - a crash report file in the user-data dir (`GetUserDataDirectory`);
  Win32 minidump via `MiniDumpWriteDump` (dbghelp) as the richer artifact.
- **Symbolication** - resolve frames to symbols (DEBUG builds carry them -
  [[dev-build-config]]); in-process `backtrace_symbols` now, offline addr2line on
  raw offset+build-id as the robust path.

Scope for the week: P0 = install the handler + write a stack trace + log tail +
build/version to the user-data crash folder on POSIX and Win32 (raw frames if
symbolication is not ready). Win32 minidump + any upload/telemetry are follow-ons
(upload is an outward-facing decision - keep local-only by default).

Open questions for week start:
- Symbolication path (in-process vs offline addr2line on offsets + a stored build
  id) and whether dist strips symbols.
- What the report contains (stack, log tail, version, OS/GPU info) + the format.
- Editor AND exported player both install it (the player is where field crashes
  happen); web/Emscripten is out of scope (browser handles it).
- Signal-safety discipline on the POSIX handler; re-entrancy / double-fault guard.

Tests: the report WRITER formats + writes a given synthetic stack/context + log
tail correctly (headless); install/uninstall of the handler is clean; a
dev-only "force crash" command verifies end-to-end manually (a real crash is not
unit-testable).

## Seeded: mountable content directories (user 2026-08-29)

Origin: reviewing Sedulous's VFS while building the editor's `data://` discovery+mount. Sedulous lets
a project MOUNT MULTIPLE content directories. The ask: allow an editor project to mount additional
content dirs beyond its own Sources/ - so authored content can be packaged (zip a data dir), shipped
to someone else, and they DOWNLOAD + MOUNT it into their editor project (asset packs, shared prefabs,
sample content). Builds on foundation.vfs (schemes/mounts) + the content DB.

Sketch: a project references N mounted content roots (its own + external packs); the content DB and
asset browser union them; GUID stability across packs (a pack's assets keep their guids so references
resolve after mounting). Decide: mount persistence (in project settings), conflict handling (same
guid in two packs), whether external mounts are read-only, and product/cook handling for mounted
sources. Relates to the `data://` discovery work + [[vfs-and-resource-stack]].

## Carried: page-toolbar roll-out - remaining pages (from week-2026-08-29)

The PageToolbar (Save / Undo / Redo / Discard, self-gating on dirty + stack
state) landed 2026-08-31 on: sound cue (already had it), bus layout, collision
shape, texture, image. The audit corrected the old seed: NO other page carried
it. Remaining editing pages to wire (same recipe - wrap the content root in a
column with the toolbar on top, Refresh() in OnUpdate):
material, animation clip, animation graph, particle effect, font, heightfield,
terrain, input map, and the generic asset form. Gotcha from the first batch: a
page that mutates its asset DIRECTLY (no commands) must override DiscardChanges
to reload from the source DB - the base "undo the stack" default clears dirty
without reverting (collision shape needed this); pages whose edits are blob
commands (texture, image, bus layout) keep the base. Interface units that add
the OnUpdate override need `import foundation.runtime.client`.

## Seeded: real-time global illumination (user 2026-09-03)

Origin: user request 2026-09-03; design discussion in-session. Constraint that
decides everything: WEBGPU-FIRST - hardware ray tracing does not exist on the
web target, so HWRT can only ever be a native-only enhancement behind
capability flags (the RT pipeline objects in Vulkan/DX12 are dormant but
proven - Sample021/028), never the GI itself.

Recommended ladder (agreed direction, not yet signed off):
- **Tier 1 - SSGI**: "SSR for diffuse" - cosine-weighted screen rays sampling
  last frame's lit color, sky/probe fallback on miss, temporal accumulation.
  Rides the existing SsrPassImpl trace core + GTAO sampling + TAA/motion
  vectors. Small, ships on every backend, kills the flat-ambient look.
- **Tier 2 - irradiance probe volumes (the backbone)**: IrradianceVolumeComponent
  grid of diffuse probes; DDGI-style octahedral irradiance + mean/variance
  depth for visibility weighting (anti-leak) - or SH9, resurrecting the
  deferred SH9 item from [[reflection-probes-plan]]. Updates amortized
  round-robin through the EXISTING runtime probe-capture path (N probes/frame,
  time-of-day-speed dynamism, no RT needed). Dynamic objects sample the
  volume; specular stays reflection probes + SSR. Plain textures + compute =
  WebGPU-safe.
- **Tier 3 (later)**: ray-traced probe UPDATES against a scene BVH - compute
  traversal everywhere, dormant HWRT natively. Upgrades tier 2's update speed,
  not its representation.

Rejected: voxel cone tracing (poor clustered-forward + WebGPU fit, dated),
SDF-GI/Lumen-style (needs a mesh-distance-field pipeline we don't have; thin
walls leak; live terrain sculpting would invalidate the global SDF
constantly; the SDF only answers WHERE a ray hits - a surface-radiance cache
as big as the SDF layer is still needed on top).
Radiance cascades = watching brief only.

SURFEL-CACHE GI (GIBS/SEED family) - evaluated 2026-09-03, the candidate
EVOLUTION of tier 3 (not a step before it): screen-spawned world-persistent
surfels accumulating irradiance via rays; no content pipeline, no thin-wall
problem, dynamic/skinned-friendly, ~MBs of memory, multi-bounce ~free
(surfels sample surfels), ray count scales with surfel count not resolution
(software-BVH friendly, WebGPU-feasible: compute + atomics + indirect).
Gated on the scene BVH (shared with the lightmap baker - build once, use
thrice) and on debug views (surfel systems fail by boiling/flicker, hardest
class to debug blind). It COMPOSES with tier 2, never replaces it:
never-seen areas have no surfels, so probe volumes stay as the fallback
layer; specular stays SSR + reflection probes. Decision point: once the BVH
exists, choose RT-updated probes vs surfel cache over probe fallback.

Open rulings for week start: probe encoding (DDGI octahedral+depth vs SH9),
volume placement/authoring UX, SSGI half-res vs full-res, where the indirect
term lands in the forward shader (replaces ambient for lit pixels).

## Seeded: lightmap baking (user 2026-09-03)

Origin: user request 2026-09-03; design discussion in-session. LAST in the
decided order (after debug views -> SSGI -> probe volumes). A PIPELINE
feature more than a renderer one - the cook substrate (content-hash recipes,
headless CLI, WriteData sidecars) and the nav-bake editor precedent (progress
UI, parallel bake, partial rebake, stage viz) are exactly the needed shape.

Recommended shape (not yet signed off):
- **UV2 unwrap**: vendor xatlas (MIT - fits the license rule). Chart
  generation at model import or bake time; meshoptimizer does not unwrap.
- **Baker core**: GPU compute progressive path tracer (Godot/Bakery-style):
  lightmap-space G-buffer raster (position/normal per texel via a UV2-space
  pass), CPU SAH BVH of the static scene uploaded to compute, accumulate
  bounces, dilate, a-trous denoise. Host-side tool = Vulkan/DX12 regardless of
  the web runtime; dormant HWRT can accelerate later. AVOID vendoring embree
  (heavyweight; collides with [[vendored-binary-portability]]).
- **Directionality**: L1 SH lightmaps (~4 textures, Witness/Bakery-style) so
  normal maps survive on static geometry. RGBA16F first; this is the concrete
  forcing function for the BC6H gap in the parity doc.
- **Runtime**: per-instance UV2 atlas scale/offset + "lightmap static" flag on
  MeshRenderer; forward shader substitutes lightmap irradiance for
  probe/ambient diffuse on static instances. Dynamic objects sample the GI
  tier-2 probe volume - and the SAME baker bakes those probes, so static and
  dynamic agree.
- **RULING NEEDED**: GPU path tracing is not byte-deterministic across GPUs,
  which collides with the cook's same-input->byte-identical philosophy.
  Proposed: lightmaps are an AUTHORED ARTIFACT (explicit bake action, stored
  source-side like the baked navmesh), not a cook product - cook stays
  deterministic, bake uses whatever hardware is present.

DECIDED ORDER (user 2026-09-03) across all four renderer seeds:
1. debug views -> 2. GI tier 1 (SSGI) -> 3. GI tier 2 (probe volumes) ->
4. lightmap baking LAST. Runtime probe updates / RT acceleration remain a
later follow-on; the baker's scene BVH seeds GI tier 3 - build once, use
twice.

## Backlog absorbed from roadmap.md (archived 2026-09-01)

The engine roadmap synced to reality and moved to Documentation/Archive/
roadmap-history.md; everything still open there lives HERE now (the weekly docs
are the one planning surface). Grouped by track, each with its trigger/state:

**Core hardening (build when a consumer appears)**
- Local convenience test-runner (one command -> all ctest presets).
- SIMD hot-path migration - MEASURED and deferred: the naive swap was ~1.05x;
  a real win needs data kept in SIMD across a whole hot loop. Profile-gated.
- Concurrent / lock-free containers (only the JobSystem's internal deque exists).
- Allocators: general-purpose heap (TLSF/buddy) + debug guard-page/quarantine.
- Unicode normalization + collation (basic decode/encode only today).
- Pak compression (zstd/lz4; kCompressionNone today) + runtime write/watch.

**Rendering**
- Texture cook tail: BC6H for HDR + the normal-map format fix (SPEC READY:
  texture-compression-hdr-and-normals.md, ships after texture-page-ux.md) and
  KTX2/basis import (the WASM/Android transcode path).
- GPU memory sub-allocation: Vulkan -> VMA, DX12 -> D3D12MA (one allocation per
  resource today; the vkAllocateMemory-FAILED warnings stay as the tripwire).
- Spatial acceleration STRUCTURE (BVH/octree) - brute-force sphere culling
  shipped; dense/shadowed scenes are the trigger.
- Post-processing phases 4/5 - phases 1-3 built and PAUSED pending the user's
  on-screen verify; 4/5 resume after.
- DX12 blit pipeline TODO (needs D3DCompile, DxDevice.cppm) - Windows-side.
- GPU-compute particle sim (CPU sim + full authoring triad shipped).
- Scene streaming / partitioning.

**Subsystems**
- Behavior trees (navmesh/pathfinding/crowd complete; this is the AI half).
- Networking tail: per-field bitmask delta, receive-RPC-into-script,
  project-settings -> NetworkStartup, Win32 socket backend validation; the
  net-extraction spec (P1-P4) is written and unbuilt.
- Save-game (settings/config + i18n foundation shipped; save-game is the gap).

**Portability**
- Web tail: web networking (WebSocket client), Firefox validation, export
  integration ([[web-platform-track]] carries the state).
- Android: shell backend + touch/input + build (Vulkan covers the GPU side).

**Editor + MVP closer**
- Asset browser: drag-from-assets + importer chooser (thumbnails shipped).
- Export-depth hardening + ONE real end-to-end export smoke - the last MVP gap.
- Integrated showcase sample: PaperKid covers input/physics/audio/scripts/
  prefabs/game-UI; a networking showcase remains (net demo scripts are the seed).

## Backlog absorbed from navigation.md (archived 2026-09-01)

The navigation track doc moved to Documentation/Archive/navigation-history.md
(COMPLETE + review PASS; terrain surfaces feed the bake since 2026-08-31).
Still open, each with its trigger:

- **Editor on-screen verify (user sign-off owed)**: the P4b Bake button + zone
  gizmo + inspector flow, exercised in a real editor session.
- **Player-path nav demo**: a cooked scene with agents loaded by DraconicPlayer
  (demo B) - rides the authored-editor-sample item, not nav work itself.
- **Async bake**: BakeNavigationZone stays SYNCHRONOUS by ruling; go async
  (job system + progress) only when a MEASURED bake stutters (>~100ms on a real
  zone). The call site is one function, so the move is mechanical.
- **Interactive box-extents gizmo handles**: a SHARED problem - decals, probes,
  and nav zones all want the same handles. Build ONE shared helper for all
  three when any of them needs it; never a nav-specific one.
- **NavigationTool (viewport tool + panel)**: the genuine trigger is modal
  authoring - off-mesh LINK placement (jump/ladder) or walkable-AREA painting.
  Build the tool then, on the parked IViewportTool + tool_panel seams.
- **P2 features**: cross-zone stitching/portals, dynamic obstacles
  (DetourTileCache - or possibly just RebakeNavigationZoneRegion triggers,
  which shipped 2026-09-02 with the full partial-rebake chain), NavBlocker
  volumes, MCP `nav_bake` tool (rides the MCP resume list).
  Terrain-as-bake-source is DONE (0c05ad89), the bake is TILED + PARALLEL
  (toggleable) and partial rebake is wired end to end - do not re-plan
  any of those.
- **Lumix nav parity round: ALL FOUR DONE 2026-09-02** - full record in
  week-2026-08-29.md (per-agent speed/stopDistance, agent introspection,
  tiled bake + per-tile regen, bake stage viz). USER VISUAL VERIFY owed
  (agent labels, stage overlay, tiled zone in a real session).
- **Sequencing rule already recorded in terrain-backlog.md**: terrain HOLES
  must subtract their cells from the bake when hole cutting lands.

## Backlog absorbed from docking-v2.md (archived 2026-09-01)

The docking-v2 doc moved to Documentation/Archive/docking-v2-history.md (BUILT
2026-08-24: Linux OS-chromed floats, drop preview + tab chip, close-veto,
stuck-drag watchdog). Still open:

- **BUG - secondary-window viewport render**: a detached viewport only draws
  while the main window is visible. The window loop is ruled out (all windows
  render uniformly - verified); suspects in order: the page's offscreen scene
  render scheduling after a float (host rebind), present/acquire when the
  primary is occluded, UIHost single-window update routing starving the
  float's layout. Needs a live session to bisect.
- **User visual verify on Wayland**: float a panel (OS title bar + close/
  resize/snap working), re-dock via header drag (chip + drop preview, window
  stays put) and via double-click, dirty-page veto on the OS close button.
- **Optional, unprompted-by-need**: dock-zone hints from OS window-move events
  (platforms that report positions mid-move; not Wayland), and a user setting
  over SetOSChromeOverride if anyone wants borderless Linux floats back.

## Backlog absorbed from Core.md (archived 2026-09-01)

The Core implementation plan moved to Documentation/Archive/Core-history.md
(done + load-bearing; the archive stamp corrects its stale §10 - most of that
list shipped). Genuinely still open:

- **Win32 runtime validation depth**: the Win32 System/Threading backends and
  MSVC build are CI-green as COMPILATION; runtime validation on real Windows
  (incl. CaptureStackBackTrace on the assert path) has never had a proper
  pass. Rides any real Windows session.
- **Async IO**: no async file API exists; no consumer has needed one (the job
  system + sync streams cover the cook and streaming paths so far). Build when
  a measured stall names it.
- **DynamicLibrary name/extension resolution**: callers pass full platform
  file names today. Minor; build with the first real plugin consumer (the
  plugin SYSTEM itself is phase-never by MVP ruling - static-link modules).

(Lock-free containers, TLSF/guard allocators, Unicode collation, pak
compression are in the roadmap-absorbed backlog above - not duplicated here.)

## Backlog absorbed from renderer-improvements.md (archived 2026-09-01)

The renderer optimization backlog moved to Documentation/Archive/
renderer-improvements-history.md (the full measured analysis lives there; this
is the work list). Priority order preserved:

1. **Parallel command recording + draw-list sort (1a)** - THE priority. The
   Godot 4.7 A/B (same RTX 2060, shadows on) put Draconic ~40% behind per
   sphere at 144k, and the entire gap is single-threaded CPU: BuildDrawList
   sort 6.47ms, pass record single-threaded while extraction is already
   job-parallel. renderer.md section 7 pre-designed the thread-range seam;
   fill it. Target: CPU below the ~30ms GPU -> GPU-bound -> Godot parity.
2. **Shadow instance reuse across cascades (1b)** - shadow.resolve 8.96ms:
   each of 4 cascades independently re-fills InstanceData. Build caster
   instance data ONCE, per cascade emit only a DataOffsets index list (the
   prepass->forward sharing extended to shadows). NOTE the recorded
   correction: the reverted resolve-SoA attempt RELOCATED the scattered read
   instead of eliminating it - "at the practical CPU floor" was true only of
   that approach, not the shared-buffer fix.
3. **De-dup depth/forward/shadow fills (1c)** - mostly covered once 1a+1b land.
4. **MultiMesh deferrals** - compact per-instance data (144B -> 3x4 48B),
   per-instance LOD selection (pairs with GPU-driven), coarse spatial-split
   culling for huge sets, no-prepass mode for cheap-shading crowds (must stay
   a MODE - the prepass feeds SSAO/motion vectors), static shadow caching for
   static crowds.
5. **GPU-driven / indirect-draw path** - the designed-not-built seam (batches
   produce indirect args, compute cull drops in). Build AFTER 1a shows its
   ceiling; they share the submission-interface assumption.
6. **Spatial acceleration structure (BVH/octree)** - same item as the
   roadmap-absorbed entry above; drops in behind the existing cull seam
   (worldCenter/worldRadius are the ready-made index). Trigger: a profile
   where culling dominates (many objects x many views/lights). A stress-test
   no-op by construction (everything framed).
7. **Feature backlog** - SSR polish (reproject, Hi-Z, stochastic GGX, IBL
   de-double-count, metallic tint), OIT transparency, reflection-probe
   parallax/blend extensions, further post passes - as product needs pull.

Shadow knobs (distance/far-fade/per-cascade culls) SHIPPED and tunable;
remaining shadow perf is item 2 or accepting the far cascade's honest GPU cost.

## Backlog folder absorbed (Documentation/Backlog -> Archive, 2026-09-01)

All 21 Backlog docs moved to Documentation/Archive/*-history.md; still-open
items live HERE, corrected against the tree. The archives keep the full detail
(measurements, per-issue diagnoses, growth paths) - this is the index.

**Subsystem follow-ups (all consumer/demand-gated; details in each archive):**
- *Input*: per-player device pairing (awaits a split-screen consumer),
  action-triggered haptics, per-scene input.
- *Audio*: the grain-bank GROWTH PATH a-d (in-loop-out cues -> parameter
  system -> blend cues -> composite cues; grown, never big-bang ported -
  user ruling 2026-07-19). Plus issue I7: clip-page pause + volume, runtime
  volume API (small).
- *Physics*: convex decomposition (V-HACD vendoring decision), gravity
  volumes, JPH_DEBUG_RENDERER wiring, per-world Jolt job-pool consolidation,
  contact events into per-entity script handlers.
- *Scripting*: live-state-preserving hot reload (blocked on a getter
  convention), script-defined editor tooling hooks, debugger P2 profiler /
  P3 remote transport / P4 extras, luau-analyze CLI (P5b), delegate
  per-signature funcdefs (also a carried weekly item), Entity type into the
  script contract lib, Coroutine namespace completeness. CORRECTION: Wren
  retirement is DONE (backend set = AngelScript + Luau) - the archive's
  "deferred by the user" note is stale.
- *Game UI*: world-tier option-A direct draw (VG-consult-gated), dirty-gated
  world-panel redraws, atlas packing + panel MIP chains, declarative markup
  bindings, theme variants, UIDocumentPage per-line gutter diagnostics,
  two-interactive-scenes pointer routing hardening, the toolkit test tail
  (~212 upstream tests + UISandbox tabs).
- *Networking*: the full refinement list (per-field bitmask delta,
  render-rate interpolation, priority/bandwidth budget, relevancy helpers,
  quantization; congestion/encryption/authority/lockstep/IPv6/web transports/
  NAT+services later) + Win32 socket validation - the one genuinely
  UNVERIFIED surface. Supersedes the shorter roadmap-absorbed networking
  entry above.
- *VG*: overlay-tier MSAA (resolve-attachment plumbing is the real work),
  EvenOdd clip paths, nested clips - all deferred by choice post-#121.
- *Web (task #112)*: (A) WebSocketTransport + native WS accept path BUILT
  2026-09-01 - full record in week-2026-08-29.md. OWED: the user browser
  smoke (native host, the net demo recipe; browser WebScene joins,
  replicated state moves) - node cannot proxy WS so the browser proof is
  manual by design. Then (B) Firefox compat, (C) audio autoplay
  resume-on-gesture + AS browser smoke.

**Editor polish (remaining after P1/P2 shipped):** import-dialogs audit,
theming-gaps audit (hardcoded colors -> palette), authored SVG icons for the
placeholder spots, docking feel (hit zones, drag preview, re-dock hysteresis),
per-page panel-size persistence in editor settings.

**Build times (task #136):** MEASURED (profile in the archive: clean 228s,
ThirdParty ~46%, 3 chokepoint interface edges with 90+ TU fan-out); fixes NOT
started. CORRECTION to the candidate levers: ccache is BANNED with modules
(proven stale-BMI corruption) - that lever is dead; the live ones are
interface hygiene on the fat modules, module splits, mold/split-dwarf, and
ThirdParty rebuild confirmation.

**UI core (audit follow-through):** P0-P2 + the damage gate SHIPPED; remaining
= P3 styling dogfood (themes to .sss + UA default sheet + state ladder -
scoping investigated, in the archive) and any P4 leftovers noted there.

**Issues triage (user-filed, 2026-08-08) - still open:** I2 mouse-capture
never released (WATCH, hard repro), I4 memory ballooning (one half fixed;
re-verify the rest), I5 allocator plumbing DONE (TRACK
COMPLETE 2026-09-02, P0-P7 incl. the tripwire lockdown - full record in
week-2026-08-29.md),
I7 audio clip page small items, I8 Linux exported-player colors (root cause
unconfirmed - harden the swapchain fallback regardless), I10 Windows
path-separator normalization (trivial, exact fix known), I11 scene-pass MSAA
(same item as the renderer backlog). I1 collider gizmos + I6 compression are
FIXED/SHIPPED.

**Deferred-by-design (do NOT build without a user go-ahead):** GPU particle
sim, post 4/5 (1-3 VERIFIED 2026-09-03 - unblocked, still needs the
go-ahead), AS debugger P2/P3, draconic.gui
(parked entirely - the keyframes fix shipped), SIMD layout migration, web
pthreads, the WebGPU map-flush MemCompare. (Skinned crowds GRADUATED -
shipped, no longer on this list.)

**Superseded records now archive-only:** parity-2026-08 (replaced by the
Parity/ folder surveys; its MISSING list is stale - navigation, terrain,
property animation all shipped since), editor-pages-gap (the coverage gap
CLOSED - every asset type opens), gui-gaps (moot while draconic.gui is
parked), sedulous-backport (raw commit lists for the OTHER repo's back-port -
consult when that work resumes).

## Backlog absorbed from asset-thumbnails.md (archived 2026-09-01)

The thumbnail spec moved to Documentation/Archive/asset-thumbnails-history.md -
generation is DONE (13 asset types across the CPU lane + the GPU stage; CPU
tripwire 5, scene tripwire 8). Still open:

- **P4 tail**: picker-DIALOG thumbnails DONE 2026-09-01 (record in
  week-2026-08-29.md; live row refresh on OnThumbnailReady deferred until
  Function grows multicast/tokens, the UI-core P4 item). Remaining: a
  settings knob for thumbnail size, and a cache-maintenance action
  (clear/regenerate all; the cache dir is <project>/.cache/thumbs, safe to
  delete).
- **On-device pixel probe for the GPU stage**: needs an editor-host harness
  (RenderSubsystem + SceneSubsystem + resources + a cooked product) no test
  target stands up today - the one honest test gap in the track.
- **User visual verify PASSED 2026-09-03**: thumbnails confirmed working
  in-editor (record in week-2026-08-29.md).
- **Terrain thumbnail** stays icon-by-ruling (recorded in terrain-backlog.md).

## Carried from week-2026-08-29 (incomplete; moved 2026-09-07)

Still-open items from last week that had no entry here. Completed items stay in
week-2026-08-29.md as that week's record; the full write-ups are there too -
this is the list, with the section to read for each.

### Work items

1. **Editor extensibility seams** (carried since week-2026-08-22; user ruling
   2026-08-18 "ultimately must fix"): per-component inspector action-row
   registry, external gizmo-renderer registration (a provider list the
   ScenePage consults, not a global mutable registry), migrate
   Editor.Navigation onto both so Editor.Scene drops its nav imports; audit
   of the central InspectorView entries as a stretch. Read: "Editor
   extensibility seams" under "Carried from week 2026-08-22".
2. **Script backend as a PROJECT setting** (punted 2026-08-18): chosen at
   project creation, stored in project settings, gates the editor's script
   surface and which VM runs; only among COMPILED-IN backends; decide
   reload-vs-live. Read: "Script backend as a PROJECT setting".
3. **AngelScript delegate funcdef surface** (Fable, not urgent): only `Action`
   (void()) and the misnamed `ScriptDelegate` (double(double)) funcdefs
   exist; a blessed set or auto-registration per reflected delegate-param
   signature. Read: "AngelScript delegate funcdef surface is ad-hoc".
4. **Spline P2 remainder**: scatter-along-spline, particle spline opcode, road
   extrusion (P0/P1/path-follow DONE 2026-08-31). Read: "spline + spline editor".
5. **Renderer debug views remainder**: pixel inspector, wipe/compare, semantic
   coverage for terrain/sprite/unlit shaders when needed (layers 1 + 2 LANDED
   2026-09-03). Read: "renderer debug views in the editor viewport".
6. **Bone attachments** (design, triggered by the first game need):
   BoneAttachmentComponent (copy one joint's model-space pose onto an entity
   after the animation tick) + the drop-pure-joint-nodes import option, which
   RIDES the import-UX planning pass (merge-meshes). Read: "bone attachments
   + joint-node import noise".
7. **PaperKid editor-use feedback** (still open): organize the new-component
   menu; drag an instance into a group; seed input map / bus layout / UI theme
   for new projects (font, sky, primitives already seeded); Shift+up/down
   multi-select bug; WASD default input map; detached window does not render
   its viewport unless the main window is visible.
8. **Asset-variants sign-off (a)**: the web pak selection is MANIFEST-FREE
   (pak-name convention) - needs the user ruling, or a manifest gets built.
   (b) browser/mobile functional check of the variant paks = user UAT.

### Verifications owed (user visual)

- Texture page UX pass: profiles row on top, Content then Sampling, the lint
  on a Normal + sRGB combination, Cooks-to updating with usage/compression.
  Cooks-to now reads "BC6H" for HDR assets (2026-09-07).
- Screen-Space GI on a lit scene (coloured wall near a floor bleeds; stable
  under camera motion) + the grain retest, still and moving.
- The remaining smoke-checklist property-animation items not covered by the
  live UAT rounds.

### Small follow-up

- ApplyProjectUiDefaults recreates the UI ResourceFontService on every
  cook-finished callback ("default font bound" log churn) despite documenting
  itself idempotent: make SetDefaultFont early-out on the same Font product.

## APPROVED, QUEUED: acceleration-structure sizing (user 2026-09-07 - "keep it in the weekly, don't do it yet")

Raptor's CreateAccelStruct allocates a FIXED 256 KB buffer (VkDevice.cppm
init(..., 256 * 1024)) and nothing anywhere calls
vkGetAccelerationStructureBuildSizesKHR: AccelStructDesc carries no geometry,
so creation cannot size, and BuildBottom/TopLevelAccelStruct never validates
the AS or scratch capacity. Small builds fit the floor and work (the Beef
port floored at 1 KB, which is why ITS builds were rejected); anything past
256 KB fails silently at build. Ray tracing has NO Raptor test - the recording
-only test the agent cites is the Beef one. PROPOSED (needs the user's call,
it changes the shared RHI surface and touches DX12 which cannot be built
here). Design APPROVED by the user 2026-09-07; scheduling deferred - do not start
until asked. (1) Device::QueryAccelStructSizes(const AccelStructBuildDesc&,
AccelStructSizes& out) returning {structureBytes, scratchBytes,
updateScratchBytes} - Vulkan via vkGetAccelerationStructureBuildSizesKHR,
DX12 via GetRaytracingAccelerationStructurePrebuildInfo, Null returns a
deterministic estimate, Validation forwards; (2) AccelStructDesc gains
`u64 sizeBytes` (0 = the backend's floor, kept for the existing callers);
(3) Build* validates dst capacity + scratch size against the query and
reports (Validation layer error), instead of letting the driver reject.
Then a Vulkan probe: one triangle BLAS + one-instance TLAS build + trace.

## Seeded: depth-ordered transform update + per-phase system lists (user 2026-09-20)

Origin: the pre-Beef "Assiduous" experiments (C# and Beef rewrites of Raptor's core, in
/home/robert/Dev/CS/GameEngine/{CSharp,Beef}/Assiduous, notes in
/home/robert/Dev/CS/GameEngine/ARCHITECTURE.md sections 11.3 and 11.4). That tree may go
away, so the whole design is written down here. The agent of the time called it "the one
considered divergence" from Raptor's scene and reported it "built, and it removed more than it
added". NOT started on Raptor; the user asked what the remark was, then to file it.

### What Raptor does today (Scene::UpdateTransforms, Foundation/Scene/SceneImpl.cpp)

`TransformData`, one per entity slot in a parallel array: `Transform local; Float4x4 worldMatrix,
prevWorldMatrix; EntityHandle parent, firstChild, lastChild, nextSibling, prevSibling; bool
dirty, updatedThisFrame`. Intrusive doubly-linked sibling lists with head AND tail on the parent
and back-pointers, so append and remove are O(1). Three details each encode a bug already paid
for, and any replacement must keep them:
- `effectiveActive`, a cached bit = the entity's own active flag AND every ancestor's, on the
  entity slot, recomputed by a pruning subtree walk at the three choke points that can change
  it (set-active, reparent, creation); ALL runtime gating reads it, never the raw flag.
- Dirty marking cascades DOWN the subtree and UP to the root. Up exists only because the update
  scans for dirty TOPS, and an ancestor must be marked to be found.
- Two passes. Pass one walks last frame's updated-index list and, for entities now clean, copies
  world into previous-world: a stopped object reports a zero motion vector instead of a stale
  smear (TAA/motion blur). Pass two is a linear scan of every slot for alive dirty nodes whose
  parent is absent or clean, recursing depth-first from each; not roots-only because a freshly
  REPARENTED entity under a clean parent is a top a roots-only scan misses (the symptom was
  pasted/duplicated children rendering at the origin).

### The experiment's design (TransformHierarchy.cs / TransformHierarchy.bf)

Entries kept SORTED BY DEPTH: `mByDepth[depth]` is a list of node indices; each node records
`Depth` and `BucketSlot` (its position in its bucket) so removal is O(1) swap-back (move the
last entry into the slot, fix that entry's BucketSlot, pop). `AddToBucket` grows the bucket
list as needed. `SetParent` = Detach, Attach, `Redepth(index, parentDepth + 1)` which removes
and re-adds the node and recurses over the subtree (each child depth + 1), marking each dirty.
Roots sit at depth 0. The update, verbatim in shape:

```
Update():
  for index in updatedLastFrame:            // pass one, as Raptor's
    if !alive: continue
    if dirty: continue                       // still moving; keep last frame's
    prevWorld[index] = world[index]; updatedThisFrame = false
  updatedLastFrame.clear()
  for depth in 0..byDepth.count:            // pass two: one forward pass, parents final first
    for index in byDepth[depth]:
      if !alive: continue
      parentUpdated = parent != None && nodes[parent].updatedThisFrame
      effectiveActive = active && (parent == None || nodes[parent].effectiveActive)
      if !dirty && !parentUpdated: updatedThisFrame = false; continue
      prevWorld[index] = world[index]
      world[index] = parent == None ? local.ToMatrix() : local.ToMatrix() * world[parent]
      dirty = false; updatedThisFrame = true; updatedLastFrame.add(index)
MarkDirty(index): nodes[index].dirty = true   // ONE node; no cascade either way
```

What that removes from Raptor's version: (1) the UP-cascade in MarkDirty (a node updates when it
is dirty OR its parent updated, and the parent's flag is already final - the top-scan the
up-cascade served is gone); (2) the DOWN-cascade too (descendants notice through parentUpdated);
(3) the choke-point recomputation of effectiveActive (own AND parent's, in the same pass);
(4) recursion (a 2000-deep chain is a flat loop; no stack-depth concern); and it gives
sequential memory access per bucket instead of chasing firstChild/nextSibling, and each depth
level is independently parallelisable (a depth's nodes read only the previous depth's
results).

What it costs: depth order must be maintained across reparenting (the Redepth subtree walk -
comparable to Raptor's reparent, which already walks the subtree for MarkDirty and for
effectiveActive); per-node bucket bookkeeping (Depth, BucketSlot, one list per depth); and
effectiveActive is recomputed for every alive node every frame where Raptor caches it and only
touches it at the choke points. The forward pass still VISITS every alive node per frame, as
Raptor's top-scan does, so the complexity class is the same; the win is the removed cascades,
the access order, and the parallel option. Honest expectation: pays on deep hierarchies and
large crowds (AnimatedCrowd, AnimStressTest), neutral on flat scenes.

### The second remark, phases (ARCHITECTURE.md 11.4)

Raptor's `Scene::RunPhase` iterates every sorted system and calls `OnUpdate(phase, dt)` on all
of them; each system's body early-returns for phases not its own. Five phases run per frame
(PreUpdate, Update, AsyncUpdate, PostUpdate, PostTransform; TransformUpdate is the scene's own,
Initialize/Cleanup are not RunPhase'd), so a system that serves one phase takes four no-op
virtual calls per frame. The experiment registers each system's phases at add time and keeps
one list per phase. Small and free. It also notes `AsyncUpdate` is a name only on Raptor:
nothing in the scene update runs in parallel (the job system is used for render extraction and
command recording only).

### If picked up

- Gate: measure first. AnimStressTest and RenderStressTest on the clang-reldbg lane, transform
  update time and total frame, before and after; a synthetic deep-chain case (2000 deep) and a
  wide-crowd case (50k roots) in Scene.Tests as the unit-level timing probes.
- Keep the three paid-for behaviours as tests: stopped object -> zero motion vector; reparent
  under a clean parent updates the same frame; effectiveActive gating unchanged for every
  consumer (the entity-active-state rule: every tick/extract loop gates on it).
- The per-phase lists are a separate, smaller commit; do it first, it changes no semantics.
- Scope decision for the user: transform update only, or also the AsyncUpdate promise (a
  parallel per-depth-level pass is the natural first use of it).

## DONE 2026-09-22: vegetation distance fade is per CHUNK - visible quads thin and vanish (user 2026-09-22)

Built the same day once the Beef port had caught up: the sketch below, as written (rank in the
tint alpha, the window on a private view slot per faded set, the CPU prefix kept as the bound).

Observed: zooming out, vegetation thins and disappears one sharp chunk-shaped quad at a time.
Cause (read, not fixed - the Beef port references the current code; HOLD): the fade is
evaluated once per (layer, chunk) set in `TerrainVegetationComponentManager::ExtractLayer`
(`VegetationComponentsImpl.cpp:496`): `distance = max(0, |origin - chunkCenter| - radius)`,
`DensityAtDistance(distance, fadeStart 40, fadeEnd 80)` -> `FadePrefix` draw count, and the
whole set is dropped past fadeEnd. Every instance in a chunk shares that one density, and
neighbouring chunks differ, so the seam between a 30% chunk and a 100% chunk is a straight
line; with a 64-sample chunk of 1 m cells (RaptorUAT: 257 samples over 256 m) the chunk is 64 m
wide against a 40 m fade window, so a single chunk spans the entire fade. The prefix design is
right (no re-upload, stable ranks); the granularity of the DISTANCE is the bug. Terrain's own
chunk LOD went through the same lesson and moved to a per-chunk coverage metric with
hysteresis and skirts; vegetation needs per-instance, not per-chunk.

Fix sketch: keep the CPU prefix as the coarse upper bound (it uses the chunk's NEAREST
distance, so it never removes an instance a finer test would keep - the prefix is exactly the
ranks below `f(nearest)`), and move the real fade into the instanced vertex shader: each
instance's own distance to the camera, the instance's own rank (store the scatter rank in
`InstanceData.Tint.a`, or hash the world position when a producer has no rank), collapse the
instance to zero scale when `rank > DensityAtDistance(d_i)`. fadeStart/fadeEnd ride a per-draw
constant (the wind lanes are the precedent for per-material data; a per-layer pair is the
cleaner home). Chunk-level drop past fadeEnd stays. Cost: a few ALU per vertex, no CPU or
upload change; result: a smooth per-instance dissolve with no chunk seams at any window.
