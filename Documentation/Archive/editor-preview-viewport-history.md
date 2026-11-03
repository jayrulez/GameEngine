# Shared editor preview viewport (extract the triplicated bespoke-page 3D preview)  (archived)

> Status: ARCHIVED - fully built. Non-authoritative: this is the original build spec, kept
> as the record of what was built and why; present-tense truth is the code + tests.

Status: COMPLETE (2026-08-18). All commits landed, both compilers green, per-page
tests unchanged (Editor.Scene.Tests 708 + Editor.Camera.Tests 25). The only
EnsureViewportBound in the tree is now PreviewViewport's; ScenePage/GamePage keep
their own loops (real edited/game scene, Fable carve-out) and Editor.GameUI's
UIDocumentPage is a 2D UI-document preview (renders the game UISubsystem, not a 3D
scene) - both correctly out of scope. Track:
prereq (host/uiHost) -> (a) Editor.Camera + Editor.Preview extract ->
(b) collision preview -> PIMPL hygiene -> (c) mesh -> (d) material ->
(e) particle -> (f) animgraph -> (g) skeleton -> (h) clip. PreviewViewport grew
three page-driven knobs during migration: SetSimulationEnabled (particle runs live;
the others pose statically), SetTimeScale (particle speed slider), SetClearColor
(darker fields). Was: APPROVED TO BUILD (Fable ruling 2026-08-18, decision delegated
by the user - "for your call"; graduated Ideas -> Specs accordingly). Was: DESIGN
QUESTION for Fable (Opus, 2026-08-18). Origin: the bespoke-pages UAT
cluster (week-2026-08-15) - specifically "Collision mesh page: no preview". Building
that preview surfaced that the 3D-preview scaffolding is already triplicated and that
Editor.Physics cannot reuse it. The user notes Sedulous landed on a shared preview
viewport and considers that the right call; this poses the extraction for a ruling.

## The finding (verified 2026-08-18)

Three bespoke pages in Editor.Scene each HAND-ROLL the identical 3D-preview
scaffolding - same members, same methods, same render loop:

- `MeshPage`, `AnimationClipPage`, `SkeletonPage` each declare:
  `RefPtr<ui::viewport::ViewportView> m_viewport`, `scene::SceneManager m_sceneManager`
  (its own preview scene group), `engine::render::RenderSubsystem* m_render`,
  `EditorCamera m_camera`, `UniquePtr<shell::InputRouter> m_router`,
  `graphics::RenderWindow* m_hostWindow`, and `void EnsureViewportBound()`.
- Each `OnUpdate` runs the same loop: EnsureViewportBound -> SyncInputRegion ->
  m_router->Update -> (hovered/focused ? m_camera.Update) -> build a `ViewCamera`
  from the camera -> `m_render->RenderScene(scene, viewport target, format, w, h,
  cameraOverride)`, with debug overlays drawn into `DebugScene(scene)`.
- `EditorCamera` (EditorCamera.cppm, ~194 lines, foundation-only: imports just
  foundation.core + foundation.shell) is a PRIVATE `editor.scene` partition
  (`import :camera`). Nothing outside editor.scene can use it.

The consequence that triggered this: the `CollisionShapeEditorPage` lives in
Editor.Physics, which links NONE of the preview stack, and its factory is not even
handed host/uiHost. To give it a preview I must either copy the ~150 lines a FOURTH
time (and add a local camera, since EditorCamera is unreachable) or extract the shared
piece. The user (and the Sedulous precedent) favor extraction.

## Proposal

Extract a shared **PreviewViewport** helper that owns the whole 3D-preview substrate,
so a bespoke page CONTAINS one instead of re-implementing it:

- Owns: the `ViewportView`, a per-page `SceneManager` + preview scene, the
  `EditorCamera`, the `InputRouter`, the host-window binding, and the
  EnsureViewportBound + per-frame camera-update + `RenderScene` loop.
- A page: constructs a PreviewViewport (from host + render/scene subsystems), adds its
  own entities to `preview.Scene()` (mesh, light, ...), calls `preview.Update(dt)` each
  frame, and mounts `preview.View()`. An optional draw hook (or the page drawing into
  `render->DebugScene(preview.Scene())` itself) covers overlays - the skeleton wireframe,
  the collision outline, the navmesh, etc.
- `EditorCamera` graduates out of the editor.scene partition into the shared module so
  it stops being editor.scene-private (it is already foundation-only).

This kills the triplication, unblocks Editor.Physics (and any future bespoke page), and
is the shape the user reports Sedulous converged on.

## Open questions for Fable

1. **Where does the shared module live?** editor.core is the headless editor domain (no
   render/shell) - it likely CANNOT host a render+shell-dependent viewport helper without
   dragging heavy deps into a deliberately-light lib. A NEW small module (editor.preview /
   editor.viewport-preview) that both Editor.Scene and Editor.Physics link seems right.
   Confirm the home + that it does not violate the editor-core-stays-light intent.
2. **Draw-overlay seam.** Should PreviewViewport expose a `Function<void(DebugDraw&)>`
   draw hook, or just hand out `Scene()` + let the page draw into `DebugScene` itself (as
   the pages do today)? The latter is less API; the former is more encapsulated.
3. **Migration order.** Extract + adopt in the NEW consumer (the collision page) first -
   zero risk to working pages - then migrate mesh/clip/skeleton incrementally? Or migrate
   all three in the extraction commit (bigger, proves the API against all real consumers at
   once, per the "design against real consumers" rule)?
4. **Factory threading.** The collision page's factory + RegisterCollisionShapeEditor must
   start taking host/uiHost (they do not today) to reach the subsystems. In-scope for this
   work, or a prerequisite commit?
5. **Naming** of the helper + the module.

## Tentative recommendation (Opus, for Fable to challenge)

- Extract into a new lean module both editor libs link; promote EditorCamera into it.
- Hand out `Scene()` + `Update(dt)` + `View()`; let pages draw overlays into `DebugScene`
  as they already do (question 2 = the low-API option) unless Fable wants the hook.
- Adopt in the collision page first (new consumer, no regression risk), THEN migrate the
  three existing pages in a follow-up - but design the API against all four up front so it
  is not shaped by one consumer (the seam-rot lesson from navigation-editor-ui.md).

Not blocked otherwise: the rest of the bespoke-pages cluster (the graph-page param editors,
the ports bug) does not depend on this; only the collision preview does.

## Not to be confused with

- `Documentation/Archive/camera-preview-history.md` (task #118, BUILT) - the floating in-viewport
  live preview of a SELECTED CameraComponent's view. Different feature; this is about the
  bespoke-page authoring viewport substrate.
- The week-2026-08-22 seeded "editor extensibility seams" item (inspector action-row +
  gizmo-renderer registries). Orthogonal: that makes per-DOMAIN surfaces plugin-extensible;
  this deduplicates the per-PAGE 3D-preview scaffolding. They could land in either order.
- Today only `DrawSkeletonWireframe` (a free draw helper in :animation_graph_page) is
  shared across pages - the viewport + scene + camera + render loop is NOT.

## FABLE RULING (2026-08-18) - approved; build to these answers

The finding is verified (the three EnsureViewportBound copies, the
foundation-only EditorCamera trapped in a private editor.scene partition, the
host-less collision factory) and the extraction is the right call - this is
real triplication with a fourth consumer in hand, not a speculative seam. The
seam-rot rule does not apply here: FOUR real consumers exist TODAY. Answers:

1. **Home: a NEW lean module - `Code/Editor/Editor.Preview`, module
   `editor.preview`, target `Editor::Preview`.** Confirmed editor.core cannot
   host it (deliberately headless; a render+shell+viewport dependency would
   wreck that). Do NOT fold it into editor.viewporttools either - that module
   is the scene-page interaction-mode framework; this is a self-contained
   page substrate. Different concerns, both stay lean. EditorCamera graduates
   into editor.preview (it is already foundation-only, so the module's
   interface stays light); editor.scene links editor.preview and deletes its
   :camera partition - the scene page keeps its OWN viewport loop (it has
   tools/gizmos/game tabs; it only shares the camera). No cycle:
   editor.preview imports foundation + engine only, never editor.scene.
2. **Overlay seam: the low-API option** - hand out `Scene()` / `Update(dt)` /
   `View()`; pages draw into `render->DebugScene(preview.Scene())` exactly as
   they do today. This is not just less API: the 6dbdfbf7 keyed-view fix
   pinned the contract (DebugScene = drawn in every view of the scene), and a
   preview scene has exactly ONE view, so DebugScene is unambiguous here. A
   Function hook would be a second way to do the same thing with no added
   capability. If a future consumer genuinely needs per-view-only chrome in a
   preview, DebugView(viewport key) already exists - no PreviewViewport API
   needed even then.
3. **Migration order: collision page first, then ALL THREE existing pages -
   in the SAME track, one page per commit, battery each.** The tentative
   recommendation's "follow-up" framing is the one thing I reject: an
   extraction that leaves the triplication standing has achieved nothing, and
   an API validated by one consumer is exactly the seam-rot trap. The three
   existing pages ARE the API's proof - if one cannot absorb PreviewViewport
   without hacks, the API is wrong and gets fixed while the work is fresh.
   Design against all four up front (as proposed); land as: (a) module +
   EditorCamera graduation, (b) collision page adopts (the zero-regression
   new consumer), (c/d/e) mesh, clip, skeleton migrate one commit each, each
   deleting its hand-rolled copy. The track is DONE when the only
   EnsureViewportBound in the tree is PreviewViewport's.
4. **Factory threading: a PREREQUISITE commit.** Widening the collision
   factory (+ RegisterCollisionShapeEditor) to take host/uiHost is mechanical,
   independently correct (future bespoke pages need it regardless - the
   scene-page factory precedent), and keeping it out of the extraction diff
   keeps that diff reviewable.
5. **Naming**: class `PreviewViewport`, module `editor.preview`, target
   `Editor::Preview`, files PreviewViewport.cppm (+Impl per module hygiene if
   the bodies are heavy). Full descriptive names, no abbreviations.

Tests ride the extraction: the module gets its own Tests target
(PreviewViewport gets a headless construct/scene-ownership/update-tick test on
the NullDevice precedent), and each migrated page's existing tests keep
passing unchanged - that invariance IS the migration test. Battery both
compilers per landing, including the full sweep (the pass-11 lesson).

Scheduling: this unblocks the CURRENT bespoke-pages UAT item ("Collision mesh
page: no preview") - proceed now under that cluster; do not park it behind the
week-2026-08-22 seeds (confirmed orthogonal).

## OPUS CORRECTION (2026-08-18, before build) - the triplication is SIX-fold

The original finding said three pages (mesh/clip/skeleton); a full sweep before
building shows SIX pure-preview pages own a private preview SceneManager + the
identical EnsureViewportBound/RenderScene substrate:
`MeshPage`, `AnimationClipPage`, `SkeletonPage`, `MaterialPage`
(u8"material.preview"), `ParticleEffectPage` (u8"particle.preview"),
`AnimationGraphPage` (u8"animgraph.preview"). `ScenePage` and `GamePage` own NO
preview scene (they render the real edited / running scene) - these are the two
Fable carved out ("keeps its OWN viewport loop"), confirmed correct.

This does not change the ruling, it EXTENDS its own principle ("DONE when the
only EnsureViewportBound in the tree is PreviewViewport's", scene/game
excepted): the migration covers all SIX, one commit per page, not three. Build
order becomes: prereq (collision factory host/uiHost) -> (a) module +
EditorCamera graduation -> (b) collision adopts -> (c..h) the six migrations,
each deleting its hand-rolled copy.

## OPUS DEVIATION (2026-08-18, during build of (a)) - TWO modules, not one

Fable ruling #1 put EditorCamera + PreviewViewport in one module `editor.preview`,
reasoning the interface "stays light" because EditorCamera is foundation-only.
It does NOT: PreviewViewport's own members (SceneManager, RenderSubsystem,
ViewportView, ...) make the `editor.preview` interface pull the render + scene +
viewport + rhi + vg graph. The moment ScenePage.cppm (already the heaviest TU in
the tree) imported that combined interface just to get EditorCamera, GCC 15
SEGFAULTED in the module merger (the gcm-cluster fragility, gcc-module-interface-
hygiene memory). Clang built it fine; gcc did not.

Fix, which is also cleaner: split into TWO modules.
- `editor.camera` (target `Editor::Camera`, module `editor.camera`): EditorCamera
  ONLY, foundation-only, deliberately lean. Imported WIDELY - every scene page +
  Editor.Physics. This is what graduates out of editor.scene.
- `editor.preview` (target `Editor::Preview`, module `editor.preview`):
  PreviewViewport, imports editor.camera + the heavy render/scene/viewport graph.
  Imported ONLY by pages that host a preview (the six, post-migration; the
  collision page first). `export import editor.camera` so a preview consumer still
  gets EditorCamera in one import.

This honors the ruling's intent (camera graduates out of editor.scene, shareable
by Editor.Physics; PreviewViewport is the shared substrate; no cycle) while
keeping the widely-imported surface lean enough for gcc. The unit tests live with
the camera (Editor.Camera.Tests: LookAt aim + level horizon, basis orthonormality,
FrameBounds, RMB free-look, WASD-fly gating). Flagged for Fable; revert to one
module only if a gcc fix makes the combined interface safe.

## GCC ICE root cause CORRECTED (Fable, 2026-08-18) - it is not a size threshold

During the material migration (commit d) MaterialPage.cppm ICE'd gcc when it
imported editor.preview. Opus's first theory (heavy-TU size threshold) was WRONG.
Fable diagnosed the actual trigger: GCC 15's module merger SEGFAULTS when ONE
interface unit imports BOTH a giant same-module partition (editor.scene:inspector)
AND editor.preview. The interface named nothing from :inspector (ResourceRefEditor
is impl-only, and MaterialPageImpl.cpp already imports :inspector legitimately) -
the interface import was pure excess. Deleting that one line is the whole fix; zero
body-slimming needed.

The rule for the remaining pages (particle, animgraph, skeleton, clip): an
interface unit imports a partition/module ONLY if the interface itself names its
types; heavy partition imports (:inspector especially) live in impl units. Grep
each page .cppm for `:inspector` co-imported with editor.preview before migrating.
This is the THIRD recorded gcc-merger trigger (after heavy 3rd-party headers and
RTTI/REFLECT bodies in interfaces); the gcc-module-interface-hygiene memory carries
it. The PreviewViewport PIMPL (separate commit) is therefore hygiene, not the fix -
kept because a lean interface is the right shape for a module 6+ pages import.
