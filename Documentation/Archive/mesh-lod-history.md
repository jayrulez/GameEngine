# Mesh LOD (levels of detail)

> ARCHIVED 2026-11-02, fully built; anything it left open is in
> Documentation/Plans/weekly_backlog.md ("Left over from the specs archived 2026-11-02"). Non-
> authoritative: the original spec, kept as the record of what was built and why; the code and the
> tests are the truth.

> STATUS: SPEC PREPARED 2026-08-15; REFRESHED 2026-08-23 (still current -
> sidecar still v3, meshoptimizer still unvendored, all five decisions
> stand; see the refresh section for what changed around it). SEQUENCED
> AFTER THE TERRAIN TRACK (user 2026-08-23). meshoptimizer dependency
> USER-APPROVED 2026-08-15.
> Read alongside Documentation/Archive/asset-variants-history.md (same week's sibling;
> LOD quality tiers could ride the variant axis later, but v1 deliberately
> does not - LODs are runtime-selected, not per-platform).

## Goal

Distance/coverage-appropriate geometry so dense scenes (Sponza-class and
beyond) spend triangles where they read. Two sources of truth, one wire
format: artist-authored LOD meshes when provided, auto-generated chains
when not (marketplace/imported content rarely ships LODs).

## Decision 1 - vendor meshoptimizer (approved)

`ThirdParty/meshoptimizer` (zeux) - industry standard, dependency-free,
MSVC-native. It provides BOTH:

- `meshopt_simplify` (+ `simplifySloppy` fallback) for auto-LOD chains;
- vertex-cache / overdraw / vertex-fetch optimization for EVERY cooked
  mesh - a free win independent of LOD, shipped as its own phase (P0)
  so the dependency pays for itself before LOD lands.

Heavy headers stay in Pipeline implementation units (GCC module hygiene).

## Decision 2 - data model: LODs live INSIDE the cooked mesh product

One mesh resource carries its whole LOD chain; LODs are NOT separate
assets (no reference fan-out, no picker churn, prefabs/components keep a
single `Ref<StaticMesh>`).

- The mesh-geometry binary sidecar (v3, `kMeshGeometryStreamName`) gains a
  per-LOD index-range table per submesh: vertices are SHARED across LODs
  (simplification only drops indices); each LOD level stores its own index
  ranges. Sidecar/asset version bump in the same commit (the bump IS the
  migration; existing meshes load as 1-LOD chains).
- Per-LOD data: index ranges per submesh + a precomputed SWITCH METRIC
  (normalized screen-coverage threshold, from simplification error or
  authored override).
- v1 scope: STATIC meshes only. Skinned LODs are deferred - simplification
  must respect skin weights/influences and remap the parallel skin stream;
  priced as its own phase (P4) after the static path proves the wire.

## Decision 3 - authoring: named LODs win, auto-generation fills

- **Authored:** the model importer recognizes the `_LOD1`/`_LOD2`/...
  node-name convention (ufbx surfaces names; FBX LODGroup nodes map to the
  same thing where present). `Foo`, `Foo_LOD1`, `Foo_LOD2` collapse into
  ONE mesh asset with a 3-level chain. Import dialog shows what it found.
- **Auto:** import dialog gains "Generate LODs" (default ON for models
  above a triangle budget, e.g. 10k): target ratios {1.0, 0.5, 0.25,
  0.125} with meshopt error bounds; levels that fail the error bound are
  dropped (a chain is as long as quality allows, never padded).
- Authored + auto never mix within one mesh: authored presence disables
  generation for that mesh.
- All knobs are AUTHORED data on the mesh asset (never runtime structs).

## Decision 4 - runtime selection: screen coverage, per view, with
## hysteresis

- Selection happens at EXTRACTION (render side already carries
  `worldCenter`/`worldRadius` per MeshRenderData): projected-sphere
  coverage against the view -> pick the finest LOD whose threshold passes.
  Per-VIEW selection (split-screen/editor-vs-game views disagree freely,
  matching per-view shadows/MSAA precedent).
- Hysteresis band (~10%) on the switch metric prevents boundary popping;
  no cross-fade/dithered transitions in v1 (noted follow-up).
- Component knobs on `MeshComponent`: `lodBias` (float, inspector-visible)
  and `forceLod` (int, -1 = auto; debug + cinematic use). Reflection
  attributes per the inspector rules (displayName/category).
- Shadows: casters use the SELECTED view LOD for cascades (camera-coupled)
  and the coarsest passing LOD for the camera-independent local-shadow
  caster list - shadows never select finer than the main view.
- `InstancedMeshComponent` (crowd sets): v1 selects ONE LOD per set from
  the set's aggregate bounds (the per-set persistent buffer stays intact).
  Per-instance LOD (bucketed draws) is the deferred v2 - noted, priced
  separately, do not build in v1.
- Culling stays untouched (LOD selection composes after the existing
  sphere cull).

## Decision 5 - editor surface

- Mesh viewer page: LOD dropdown (Auto/0/1/2/...) + per-LOD triangle
  counts; wireframe toggle already exists.
- Import dialog: the Generate LODs toggle + budget; re-import regenerates.
- Debug: a per-scene debug-draw overlay mode tinting geometry by active
  LOD (rides the existing debug-draw categories).

## Phasing (for when it is scheduled)

- **P0 - meshoptimizer vendoring + cooked-mesh optimization pass.**
  Vertex-cache/fetch/overdraw-optimize every cooked static mesh (no wire
  change - same buffers, better order). Acceptance: identical rendering
  (pixel probe), measured ACMR improvement logged by the cook test.
  SHIPPED (Fable, 2026-08-23): ThirdParty/meshoptimizer (trimmed core,
  MIT, commit in its README; bc7enc vendoring pattern);
  pipeline::OptimizeStaticMeshSource in Geometry.Pipeline (impl unit only
  - the vendored header never touches the interface), called by
  StaticMeshAssetBuilder::Build. Per-triangle-submesh vcache+overdraw,
  whole-mesh fetch remap + dead-vertex compaction, non-triangle ranges
  keep order (values remapped), malformed input passes through with a
  cook warning, ACMR before/after logged. Tests: hostile-grid ACMR
  improvement, triangle-set/winding/range preservation, lines-range
  sequence preservation, compaction, idempotency, malformed guards
  (MeshOptimizeTests.cpp). "Identical rendering" is proven structurally
  (same triangle set as position triples, winding preserved) rather than
  by pixel probe - stronger and headless. Skinned meshes deliberately
  not passed (parallel skin stream permutation rides P4).
- **P1 - wire + authored LODs + runtime selection.** Sidecar LOD table,
  importer name-convention collapse, extraction selection + hysteresis,
  component knobs, mesh-page dropdown. Acceptance: authored 3-LOD mesh
  switches by distance on screen; serialize round-trip tests; 1-LOD
  compat load of pre-bump meshes.
  WIRE SLICE SHIPPED (Fable, 2026-08-23): StaticMesh gains the LOD chain
  (LOD 0 IS subMeshes - every pre-LOD consumer untouched; coarser levels
  = flattened SubMesh ranges in the ONE index buffer over the SHARED
  vertices; SubMeshesForLod slices with clamp + malformed fallback to
  LOD 0); StaticMeshSource v3 (DataVersion 2->3, gated fields, legacy
  loads as 1-LOD) carries lodCount/lodStart/lodIndexCount/lodCoverage;
  FromMesh/FillStatic round-trip the chain with validation (bad tables
  collapse to 1 LOD, never crash); the P0 optimizer is LOD-aware (each
  level reordered, chains validated before any mutation). Tests:
  MeshLodWireTests.cpp.
  P1 COMPLETE (Fable, 2026-08-23): SELECTION - per-view at the renderer's
  resolve stage, NOT extraction (one snapshot serves every view - a spec
  premise correction): pure trio LodCoverageFor (projected-sphere
  coverage; perspective / ortho via projection[1][1], m[3][3]
  discriminates; lodBias halves coverage per unit) + PickLodLevel
  (descending-threshold walk) + ApplyLodHysteresis (+-5% band), glued by
  MeshRenderer::SelectLod with per-(view pointer, item) memory (capped,
  prepass/forward agree within a frame). ALL six draw paths route chains
  through the selected level's submesh table (forward single/instanced,
  depth single/instanced, multimesh forward/depth) - the whole-buffer
  fast path draws only chainless meshes, since concatenated LOD indices
  would draw every level at once. Instanced sets pick ONE level from
  merged bounds (spec v1). KNOBS - MeshComponent lodBias/forceLod (wire
  v4, reflected with displayName/description; extraction copies).
  IMPORTER - ParseLodSuffix (_LODn, case-insensitive, _LOD0 = base) +
  AppendLodLevelFromModel (shared blob append, offset indices, per-part
  ranges, halving threshold ladder 0.25/0.125/...; part-count mismatch
  refuses the level); the import loop folds suffixed STATIC meshes into
  their base (skinned chains warn + import separately; consumed levels
  produce no asset/manifest entry). EDITOR - mesh page LOD row
  (Auto/LOD n buttons driving the preview component's REAL forceLod
  knob) + per-level triangle/threshold stat lines. Tests:
  LodSelectionTests (coverage/pick/hysteresis), importer suffix +
  append cases, MeshStatLines chain block. Shadow cascade LOD refinement
  (main-view coupling) stays P3 as specced: shadow views currently
  select with their own ortho metric - functional and conservative.
- **P2 - auto-generation.** meshopt_simplify chains at import with error
  bounds + dialog knobs. Acceptance: Sponza-class import generates chains;
  triangle-count assertions; error-bound drop case tested.
  SHIPPED (Fable, 2026-08-23): pipeline::GenerateLodChain - up to 3
  halving levels, each simplified PROGRESSIVELY from the previous level
  (cheaper + more coherent than re-simplifying LOD 0), border-locked per
  submesh (meshopt_SimplifyLockBorder - cross-submesh seams cannot
  crack), a level committed only when EVERY triangle submesh gets near
  its halving target within the error bound (stall = quality exhausted =
  chain ends; never padded), minTriangles floor, non-triangle submeshes
  repeat their LOD-0 range so every level keeps the full submesh table,
  authored chains never regenerated over, same halving coverage ladder
  as authored imports. Import dialog gains the "Generate LODs" toggle
  (default ON; applies to static meshes >= 10k triangles with no
  authored chain); toggle-count tripwire bumped 7->8. Tests: dense-grid
  chain growth + halving bounds + index validity + optimizer/fill
  round-trip, authored-wins no-op, tiny-mesh no-op.
- **P3 - polish.** Shadow-list coarsest-LOD rule, debug tint overlay,
  per-set instanced selection.
  SHADOW RULE + INSTANCED SHIPPED (Fable, 2026-08-23): per-set instanced
  selection landed inside P1 (all instanced/multimesh paths pick one
  level from merged bounds). Shadows: RecordShadowCasters gains a
  `lodView` - CASCADES pass their owning camera view, so cascade casters
  compute selection with that view's camera AND share its hysteresis
  slot (shadow geometry matches what the view draws, exactly);
  LOCAL-shadow tiles (camera-independent) pass null, and a null-view
  SelectLod on a chain returns the COARSEST level - shadows never render
  finer than any view shows. Previously shadow contexts carried a null
  view and chains cast at LOD 0 (visually fine, wasteful).
  DEBUG TINT OVERLAY SHIPPED (user unblock 2026-08-23 - do not wait on
  the gizmo-registration seam): LodOverlayGizmoRenderer, a BUILT-IN
  gizmo (the current (B) pattern; it migrates onto the external
  registration seam WITH the other builtins when that lands). Toolbar
  "LOD" toggle on the scene page (off by default); with it on, every
  chained mesh draws its bounds through the viewport's KEYED debug list
  tinted by the level THAT viewport's camera picks (green/yellow/
  orange/red), recomputed via the exported pure selection functions
  (raw pick, no hysteresis - a lens, not renderer state). GizmoContext
  gained the viewport ViewCamera + the overlay flag; gizmo-count
  tripwire 5->6.
- **P4 - DEFERRED** (revised 2026-08-23 - user challenge collapsed most
  of it): SKINNED CHAINS SHIPPED - the P0 optimizer permutes the
  parallel skinning stream with the identical vertex remap (mismatched
  stream sizes refuse the whole pass), the skinned builder runs the
  pass, authored _LODn collapse works for skinned meshes (the skinned
  append keeps both streams in lockstep; MIXED skinned/static pairs are
  refused), and auto-generation applies (simplification only drops
  indices - the per-vertex skin stream is untouched by construction).
  The renderer needed NOTHING: chains flow through the already-patched
  skinned draw paths. Still deferred: WEIGHT-AWARE simplification
  quality (meshopt simplifyWithAttributes - joints can melt at
  aggressive ratios without it; revisit when a real character shows
  it), per-instance LOD bucketing, cross-fade transitions, LOD-aware
  physics cooking (physics keeps cooking from LOD0).

## Refresh (Fable, 2026-08-23) - sequenced after terrain; three updates

Verified against the tree: the geometry sidecar is STILL v3 and
meshoptimizer is STILL unvendored, so the wire plan and P0 are current as
written. Decisions 1-5 stand. Three things changed around the spec:

1. **Terrain boundary (terrain now lands FIRST).** Terrain has its OWN
   LOD machinery - per-chunk geo-mipmapping selected by screen-space
   error (foundation.terrain's pure selection function) - and the two
   systems stay SEPARATE: terrain chunks are not mesh-LOD chains and
   never ride this spec's wire. One deliberate touch point: both compute
   a "projected size on screen" from bounds + camera. Terrain builds its
   math first; at P1 here, CHECK whether it is genuinely the same formula
   and extract a shared foundation helper only if it is - do not mandate
   sharing in advance (it may be ~20 lines each; a forced common helper
   is worse than honest duplication), and do not invent formula #2
   blindly either.
2. **Grass does NOT wait on this spec.** Terrain P3 grass renders through
   the instanced-mesh path with its OWN per-layer distance/density rules
   (fade/density falloff, not LOD chains). This spec's v1 per-set
   selection (one LOD from a set's aggregate bounds) would DEGENERATE on
   terrain-wide grass sets (aggregate bounds = the whole terrain), so
   grass must not be built on it; if grass ever wants per-instance detail
   reduction, that is the deferred per-instance bucketing (P4), priced
   then. Recorded so terrain P3 never blocks on mesh LOD.
3. **Debug tint overlay: mechanism narrowed (user correction
   2026-08-23).** Since 2026-08-15 the debug-draw contract split into
   DebugScene (scene content, drawn in EVERY view) and DebugView(key)
   (private to ONE viewport - how editor chrome stays out of the camera
   preview today). LOD selection is PER-VIEW, so the one path that is
   ruled out is the unkeyed DebugScene list (identical content in every
   view). TWO valid mechanisms remain, chosen at build time:
   (a) view-KEYED debug draw - the extractor pushes per-mesh colored
   bounds/labels into the extracting view's keyed list (zero shader
   work, existing seams; needs the render-view -> debug-key mapping the
   keyed-view fix already established); or (b) a per-view RENDER debug
   mode tinting the actual surfaces (nicest visuals, needs a view-level
   flag through the standard path). Decision 5's "rides the existing
   debug-draw categories" is superseded only in that the list must be
   the KEYED one, not the scene list.

Also noted: extraction-side selection composes inside the existing
extract loops, which now gate on IsEffectivelyActive (entity-active-state,
shipped 2026-08-18) - selection adds no new loop, so no new gating
obligation.

## Test notes

Doctest in Geometry.Pipeline.Tests (wire, chains, compat load) +
ModelImporter.Tests (name collapse, generation) + a Foundation.Render-side
selection unit test (coverage math + hysteresis, no GPU). Both compilers.
Pipeline targets stay UI-free.
