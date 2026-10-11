# Editor Infrastructure

> Status: CURRENT
> Verified: 2026-08-12 @ e9c4f8d6
> Track: [[editor-track]]

The editor is built out well past the phased plan below. Shipped: the shell (`editor.app`) +
`editor.core` (headless logic), scene pages rendering live scenes through the real renderer, per-page
hierarchy (rename, drag-reparent, sibling reorder, world-preserving reparent), Guid-based entity
commands with full-subtree undo, CPU-ray viewport picking, the reflection inspector (`SceneInspectorView`
+ `SceneEditContext` merging commands, Add/Remove component from reflected managers), gizmos
(`TransformGizmo` + `GizmoController` + component gizmos, one-undo-per-drag, W/E/R/X with camera-fly
gating), and phase 6 - the asset browser (`AssetsView`) + the cooker (`EditorCookService`). On top of
that: the project manager (`project-and-settings.md`), a broad set of bespoke per-asset editor pages
(scene, mesh, skeleton, animation-clip, animation-graph, material, texture, image, particle-effect,
collision-shape, audio-clip/sound-cue/bus-layout, input-map, UI-document, script, game), the generic
`AssetFormPage` fallback (`editor.generic`), and background jobs (`editor-jobs.md`).

The design record below (the architecture, the three-editor survey, the phased plan) describes the
system that was delivered; it is preserved for the "why".

**Inspiration:** Traktor (gold standard - source-db + pipeline + per-type editors), Lumix (fast
iteration - StudioApp + plugins, command-mediated live-world editing), Sedulous (our lineage - keep the
skeleton, fix the two known weaknesses: dead plugin layer, shallow undo coverage).

**North star:** a modular, data-driven editor leaning on the engine's strengths (full runtime
reflection, the Asset->cook->Resource triad, content DB, debug-draw, `foundation.ui`) - Sedulous's
skeleton + Lumix's mutation discipline + Traktor's asset pipeline.

---

## 1. What Draconic already has (build on, don't rebuild)

- **`foundation.editor`** - the asset-cook *authoring base*: `Asset` (source object referencing an
 external file + import settings) / `IAssetBuilder` / `DefaultAssetBuilder`, `AssetBuildContext`
 (assetRoot + output `Instance` + db for cross-refs). The source→product cook seam.
- **Content DB** (`foundation.content`) - GUID identity, Group tree, `Instance` =
 `ReadObject/WriteObject` (primary `ISerializable`) + named data streams. **Pluggable
 `SerializerFactory`** (2026-07-11): `XmlSerializerFactory` (readable/diffable **source** DB) vs
 `BinarySerializerFactory` (**cooked** DB) - the Traktor source-db/output-db split, already built.
- **Scene serialization - DONE** (memory said "pending"; it isn't): `foundation.scene.resource` has
 bidirectional `SerializeScene` (entities by GUID, parent links + component owners relinked on
 load, components serialized by their typed managers, routed by stable string type id) +
 `SceneDocument` primary; `foundation.scene.editor` has `SaveScene(scene, instance)`. The design
 comment is explicit: *"for scenes the cooked file IS the authored form"* - scenes serialize the
 **live world**. Big input to §5a.
- **Full runtime reflection** - `PropertyInfo` (name/type/flags + Variant `get`/`set`),
 base-chain `FindProperty`, methods, enum reflection, `Variant`/`Instance`. Flags today:
 `ReadOnly` only - an attribute channel (range/color/resource-ref/category) is a small add (§3.5).
- **UI framework + workbench layer - proven in UISandbox:** `foundation.ui` + toolkit
 `DockManager`/`DockablePanel` (persistence IDs + dock-layout save/restore tests),
 `foundation.ui.application::RuntimeDockableWindowHost` (floating panels = real borderless OS windows,
 drag-follow Tick), `foundation.ui.viewport::ViewportView` (offscreen color+depth RT, `OnRender`
 delegate with barriers handled, ContentFit shared draw/input math, gated `InputSurface` facades).
- **Debug-draw** (per-scene + global gizmos + text) - gizmo rendering foundation.
- **Renderer + scene/ECS + animation + particles** - viewport content done. Resource manager with
 dependency tracking + transitive reload (hot-reload seam).
- **Runtime** - `IApplication`/`IApplicationHost`, multi-window, input routing (`InputRouter`/
 `InputSurface`), UIHost.

## 2. Scope (from the roadmap → Track: Editor)

- **Reflection-driven inspector** with per-type custom editors.
- **Scene hierarchy** panel - entity tree, selection, reparent, add/remove.
- **Gizmos** - translate/rotate/scale on debug-draw, snapping.
- **Viewport** - renderer output + editor camera + picking/selection + gizmo interaction.
- **Asset browser** - over the content DB; import + cook (needs the offline cooker), thumbnails.
- **Model-import → prefab** workflow.
- **Play-in-editor** - run the sim, restore edit state.

## 3. Architecture (LOCKED 2026-07-11)

One-line formula: **Sedulous's EditorContext + registries + per-type pages, Lumix's
everything-is-a-command mutation discipline, Traktor's source/cooked-db asset cook (already
built), on foundation.ui docking with ViewportView-hosted scenes.**

### 3.1 Module layout (fixes Sedulous's god-class + dead-plugin weaknesses structurally)

- **`foundation.editor`** (exists) - Asset/IAssetBuilder cook base. Unchanged.
- **`editor.core`** (new) - **headless** editor domain, no UI imports, fully
 doctest-able: `EditorContext` (service locator), all registries, `EditorCommandStack` +
 `IEditorCommand`, selection model, project model, page abstraction (`IEditorPage` sans widgets).
 This is Sedulous's `Editor.Core` split, kept because it made their editor testable and ours has
 a tests-required rule.
- **`editor.app`** (new) - the UI shell on foundation.ui: EditorApplication (runtime
 `IApplication`), MenuBar/StatusBar/DockManager chrome, concrete panels (hierarchy, inspector,
 asset browser, console) and page hosting, layout persistence.
- **`foundation.<sys>.editor`** (per-subsystem, some exist: `foundation.scene.editor`) - each engine
 module's editor support: pages, inspectors, gizmo renderers, importers/builders. Exposes one
 `RegisterEditor(EditorContext&)` entry point.
- **Editor executable** (new app target) - the *assembly*: links the `foundation.<sys>.editor`
 modules it ships and calls each `RegisterEditor`. **The editor core/app never link engine
 subsystem editor modules OR engine subsystems** - this is the fix for Sedulous's "editor
 statically depends on every engine module" problem. (Traktor's `findAllOf` RTTI auto-discovery
 is elegant but needs enumerable RTTI + DLL plugins; the statically-assembled app is Lumix's
 `LUMIX_STUDIO_ENTRY` pattern and costs one file.)
- **Engine access from the shell is INTERFACE-only** (decided 2026-07-11, after the multi-scene
 render fix): the app drives scene rendering through `render::ISceneRenderer`, extracted into
 the light **`foundation.render.api`** module (core+rhi+scene deps only; `foundation.render`
 re-exports it, `RenderSubsystem` implements it - Sedulous's Abstractions-assembly split). The
 exe injects the pointer (`app.SetSceneRenderer(...)`) from `registerEditors` (which receives
 the `EditorApplication&`), and registers engine subsystems via the `configureEngine` hook. The
 frame shape is Sedulous's: ONE `BeginRendering`/`EndRendering` per frame around ALL pages'
 viewport renders, on the MAIN window's encoder - offscreen targets are window-agnostic, so
 floated panels' windows just sample them (their frames are UI-only). Per-view brackets corrupt
 descriptor sets (per-frame transient pools reset mid-frame) and erase other scenes' debug
 lists - never bracket per view.

### 3.2 Shell

`EditorApplication : runtime::IApplication`, exactly the UISandbox wiring: UIHost + main-window
RootView holding a vertical stack of **MenuBar / DockManager (center) / StatusBar**;
`RuntimeDockableWindowHost` assigned so panels float as real OS windows; `Tick()` per frame for
drag-follow. Panels are `DockablePanel`s with persistence IDs; layout + open-pages persistence to
the project's editor settings (dock persistence already tested in the toolkit). The editor embeds
the engine like any game app (scene/render subsystems on the runtime context) - one live engine
instance serves scene pages and preview.

**Global panels are Assets + Console ONLY** (Sedulous's split, and forced by multi-scene §3.6):
the dock CENTER is the document area where each open page docks as a closable tab; everything
scene-scoped - viewport, hierarchy, inspector - lives INSIDE the scene page, built from
standalone reusable views (fixing Sedulous's hierarchy/inspector-hardwired-in-the-page-builder
weakness without breaking its per-page model). Closing a TOOL panel (one with a persistence id:
Assets, Console) hides it - the dock keeps it registered, so View > Reset Layout and a layout
restore re-dock the same object and the shell's borrowed pointers stay valid (2026-09-23: a
closed Assets panel used to be destroyed, and Reset Layout then read its Parent out of freed
memory). Closing a PAGE panel (no persistence id) destroys it with its content.

### 3.3 EditorContext + registries (extensibility spine)

`EditorContext` = the central service object handed to every page/panel/plugin (Sedulous keep):
project, source + cooked content DBs, selection, page manager, active-page command routing, dialog
service, status sink, and the registries:

| Registry | Keyed by | Produces |
|---|---|---|
| `IEditorPageFactory` | asset type (nearest-type match) | document page (dock tab) |
| `IEditorPanelFactory` | id | global tool panel |
| `IAssetImporter` | source extension | source `Asset` instance(s) from external files |
| `IAssetCreator` | - (menu) | new source instances (File→New / context Create) |
| `IAssetBuilder` (exists) | asset type | cooked product (shared with offline cooker) |
| `IThumbnailGenerator` | extension/type | asset-browser tiles |
| `IGizmoRenderer` | component type | per-component viewport gizmos |
| `IPropertyEditor` | property/object type | inspector widget overrides |

### 3.4 Documents: pages + per-page command stacks

- `IEditorPage` per open asset, hosted as a dock tab: id, title, dirty, **own
 `EditorCommandStack`**, Save, revert-on-close. Factory dispatch by primary-object type.
- `IEditorCommand { Execute() -> bool; Undo(); TypeId(); Merge(prev) -> bool }` + command
 **groups** (Begin/End, unwound atomically) + **merge** (same-type top-of-stack, so a slider
 drag is one entry; failed `Execute` → command dropped, not pushed) - Lumix semantics verbatim,
 they're the best-engineered of the three.
- **RULE (the fix for Sedulous's shallow coverage): every mutation is a command.** Property sets,
 rename, reparent, add/remove component, entity create/destroy, asset edits. Inspector and gizmos
 call typed methods on the edit context which construct commands; nothing mutates directly.
 Undo/redo disabled across the play-in-editor boundary (play-mode commands popped on stop).
- **Generic snapshot fallback:** `SnapshotCommand` for `ISerializable` asset objects - serialize
 roundtrip = Traktor's `DeepClone` history state - so simple asset editors get correct undo for
 free without hand-written commands.

### 3.5 Inspector

- **Reflection-first property grid:** enumerate `PropertyInfo` over the base chain; Variant
 editors per type (numbers/bool/String/vectors/quaternion-as-euler/Color/enums via
 EnumReflection); multi-select writes to all targets. Edits go through `SetPropertyCommand`
 (merge on same property+targets).
- **Attribute channel (small reflection add):** range/slider, color, resource-ref (+ expected
 resource type), category, hidden, read-only (exists). Lumix's `IAttribute` list per property is
 the model; ours hangs off `PropertyInfo`.
- **Serializer-driven fallback (Traktor's trick):** for `ISerializable` types without reflected
 properties, an `InspectSerializer : ISerializer` builds the grid from the object's own
 `Serialize()` traversal (our uniform `Serialize(ar, "name", v)` API makes this cheap), and an
 `ApplySerializer` writes edits back. One `Serialize()` impl ⇒ save + inspector + snapshot-undo.
- **Overrides:** `IPropertyEditor` per property type; per-component custom sections after the
 reflected pass (how terrain tools / bake buttons appear, per Lumix).
- **Lists of structs (2026-09-21):** a reflected container property whose element is a value type
 with properties (`Array<VegetationLayer>`) gets, after its `ContainerListEditor` row, one expander
 per slot ("Layers 1: Grass" - the element's own `name` String property titles it, else the type
 label) holding the element's leaf rows: every editor the component rows use, the `Ref<>` and
 `EntityRef` pickers included. Rows and commands address the element through a
 `ComponentPropertyPath` (container property name + slot index) that `SceneEditContext::
 ResolvePropertyOwner` re-derives on every read and write (pools move), so a slot field is set,
 merged and undone exactly like a component field; a removed or reordered slot rebuilds the grid
 through the list's content-diff refresher. Nested containers inside an element wait for a need.

### 3.6 Scene editing, selection, picking, gizmos

- **MULTI-SCENE editing (user requirement, 2026-07-11): like Sedulous, unlike Lumix.** Several
 scene pages can be open at once, EACH owning its own live `Scene` (Scene-is-data makes scenes
 cheap to instantiate side by side on the engine's subsystems). Everything scene-scoped is
 therefore PER-PAGE, never global: entity selection, EditorCamera, viewport, command stack,
 play-in-editor state. Lumix's single-`World` assumptions (one global WorldEditor/selection/
 gizmo state) must NOT creep in - we take its command semantics, not its singletons.
- **Scene pages edit the LIVE Scene** (§5a decided): a `SceneEditContext` (one per page) mediates
 all mutations via commands (Lumix `WorldEditor` role); save = existing `SaveScene`;
 **play-in-editor** = serialize snapshot to memory → engine runs the same live scene → destroy +
 reload snapshot on stop, selection preserved, play-mode commands discarded (Lumix mechanism,
 Sedulous `SceneSnapshot` agrees).
- **Selection**: editor-level sets - per-scene-page entity selection (element 0 = gizmo pivot) +
 global asset selection; change events drive inspector/hierarchy/gizmos.
- **Picking (GPU, 2026-09-21)**: a click asks the renderer for the surface under the pointer
 pixel (`RenderSubsystem::RequestPick` keyed by the page's viewport; renderer.md 9.x) and applies
 the answer when it lands a few frames later - the entity whose mesh, instance, MultiMesh set or
 terrain is actually drawn there, by handle (index + generation). The CPU origin pick (nearest
 entity origin within a screen-ish radius of the ray) is the fallback for a GPU miss, so lights,
 cameras and empties stay clickable. The newest click wins over an answer still in flight; Ctrl is
 captured with the click. Tools reach it through `ViewportToolHostContext::picker`
 (`IViewportPicker`) and the pointer pixel + view size on `ViewportToolInput`.
- **Marquee (2026-09-21)**: a press on empty space dragged past 4 pixels becomes a rubber-band
 rect (drawn through the view's screen-space debug draw); release asks the GPU for every entity
 drawn inside it (one rect request; unique hits) and selects them - Ctrl adds. The click that
 started the drag is undone (its pick dropped, the selection at press restored) so a marquee
 never flickers through a click-select first. Without a picker the rect selects the entities
 whose origins project inside it. A tool switch mid-drag drops the marquee.
- **Gizmos**: `TransformGizmo` (translate/rotate/scale, world/local, screen-constant sizing, grid
 snap) drawn through the scene's debug-draw; drag = command group bracketing merged
 `SetTransformCommand`s; topmost-only selection filter so parents don't double-move children
 (Lumix lesson). Per-component gizmos via the `IGizmoRenderer` registry. **Vertex snap** (hold V,
 or the toolbar's Vertex Snap / Scene > Gizmo > Vertex Snap for every drag): a move drag puts the
 pivot exactly on the vertex nearest where the cursor meets the scene's drawn surfaces, whichever
 handle it holds, marked in the viewport; over no surface the drag is as it was. The surfaces
 answer through `ISceneSurfaceQuery` (`RaycastSurface`; the meshes' triangles on the CPU, a
 skinned mesh in its bind pose), never the dragged entity or its descendants.
- **Spline authoring (the flow, fixed 2026-09-20)**: Add Component > Spline puts a two-point
 segment along the entity's local X on the entity at its first Initialize phase (a bare component
 is seeded; a loaded one keeps its points), so the gizmo shows a curve at once; the inspector rows
 are the loop flag and a read-only point count (points are viewport-authored). Select the entity,
 pick the Spline tool: drag a point on a camera-facing plane, Ctrl+click a segment to insert at the
 closest curve position, Ctrl+click off the curve while it has no segment to place a point (a
 curve can be built from nothing, or after deleting down), Delete/X removes to a two-point floor,
 the selected point shows its tangent handles (drag promotes Auto->Smooth, Shift breaks the pair).
 One undo step per gesture. Points are entity-local; the transform places the curve.
- **Viewport**: `ViewportView` per scene page + `EditorCamera` (fly controller on the gated
 `InputSurface`, hover-gated); the page's `OnRender` renders the scene via `foundation.render` into
 the viewport RT (the thin-slice/UISandbox wiring, with the real renderer).

- **Scene toolbar shape (2026-09-24):** `[Translate][Rotate][Scale] | [World] | [Overlays v]
  [Post v][Debug v] | <tool palette> ... [Play][Pause][Stop]`. Overlays is one checkable dropdown
  for the editor's own debug draws - Grid, Entity markers, LOD overlay, Colliders - and the FPS
  readout (top-right; half-second windows of the page's update dt), all per-scene persisted
  in `SceneViewPref` - which also carries the editor camera (position / yaw / pitch / orbit
  distance) and the entity selection a closing page saves and a reopening one restores (ids no
  longer in the scene are dropped). The section is data version 4 with a v3 legacy reader: a
  keyed reader FAILS a whole section on a missing key, so keys added to a settings section
  always ride a version bump and a gate on `ar.Version()`; Post (per-view effect overrides) and Debug (render
  target views) stay their own dropdowns. The palette groups tools by `IViewportTool::Category()`
  (`GroupViewportTools`, first-appearance order = registration order): a category with two or
  more tools is ONE `ToolbarMenuButton` labelled by it, reading the active tool ("Terrain:
  Sculpt", highlighted) while one is on; a lone tool (Spline) stays a toggle. A domain that
  registers brushes gives them a category and takes one slot however many it adds.
- **Viewport tool refusals are said, not swallowed (2026-09-22):** every `IViewportTool` has an
 `IsAvailable()` predicate (a terrain brush needs a terrain) and now an `UnavailableReason()`;
 when the toolbar toggle's activation is refused, the scene page notifies the reason as a
 warning (the log + the toast) and the toggle snaps back. A brush that has nothing to work on
 says what the scene lacks, never plays dead.

### 3.7 Asset pipeline + browser

- **Source DB (XML factory)** = Traktor's source database; **cooked DB (binary factory)** =
 output database. Source `Asset` instances (external file + import settings) → `IAssetBuilder` →
 cooked instance. All exists; what's missing is the **cooker driver**: builder registry +
 asset-type routing + **incremental** (hash of source object + source file + builder version,
 Traktor's pipeline-db pattern simplified) + dependency-ordered build. Built as a library used by
 BOTH the CLI cooker tool and the editor's background build (Sedulous has no cooker - import IS
 the bake - but Draconic already committed to the cook seam; keep it).
- **Asset browser panel** over the source DB group tree: left mount/group tree + right content
 grid with thumbnails; import (file drop / dialog → `IAssetImporter` creates `Asset` instances),
 create (`IAssetCreator`), open (page-factory dispatch), build, show-in-explorer.
- **Hot reload**: after a cook, poke the resource manager (dependency tracking + transitive reload
 exist). Guard rapid recompiles with a per-path generation counter (Lumix lesson); GPU-side
 safety already covered by the bind-group version rule.

### 3.9 Project model (✅ DECIDED, user 2026-07-11)

**A project = a self-contained directory with a fixed layout + an XML manifest** (Sedulous
`.sedproj` shape on our VFS/ContentDatabase; can grow Traktor-style extra mounts later):

```
MyGame/ ← project root = what you "open"
 Project.xml ← shared manifest, committed: name, format version, default
 scene, extra mounts, cook settings. Serialized with the
 existing XML serializer (ISerializable; no bespoke format).
 Content/ ← SOURCE content DB (XmlSerializerFactory) - authored, committed
 Sources/ ← raw import sources (.fbx/.png/.wav) referenced relatively by
 Asset::fileName (= AssetBuildContext.assetRoot) - committed
 Cooked/ ← cooked DB (BinarySerializerFactory) - generated, GITIGNORED;
 delete = clean rebuild; play-in-editor/runtime load from here
 Editor/ ← per-user editor state (dock layout, open pages, camera) - 
 GITIGNORED (Traktor shared-vs-user settings split)
 .cache/ ← thumbnails + incremental-cook hash db - GITIGNORED
```

Opening a project: load `Project.xml` → mount the subdirs (+ read-only `engine://` for
engine-shipped assets) → open the two `ContentDatabase`s (source=XML, cooked=binary) → restore
per-user layout/open pages. `ContentDatabase(mount, factory, extension)` maps 1:1 onto this.
Packaging/ship cooks can target an external output dir via a cooker flag later.

**TAGGED FOR LATER (user, 2026-07-11): optional native game module.** A project may carry a
native code part that builds to a DLL the editor loads (game components/subsystems/editor plugins
live at edit time - Sedulous's `editor.App = TowerDefenseApp()` embedded-game-module path,
Traktor's module DLLs), AND a static-link build must remain possible (Traktor supports both;
Lumix's `STATIC_PLUGINS` vs `setStudioApp` DLL entry is the same dual). Design later (see §5
deferred); the manifest reserves a `nativeModule` field, and §3.1's `RegisterEditor(EditorContext&)`
entry-point convention is deliberately DLL-compatible (one C-linkage export resolving to it).

### 3.10 Editor logging (✅ DECIDED 2026-07-11 - improved over Sedulous; lands EARLY, with phase 2)

Sedulous's design (surveyed): a custom `EditorLogger : BaseLogger` replacing the app-wide logger
(console color output + listener notification ON THE LOGGING THREAD) + an `EditorLogBuffer`
listener (thread-safe pending list, drained to the `LogView` once per frame on the main thread,
retained UNBOUNDED until the view connects) + a good `LogView` (recycled list rows, per-level
filter toggles, 1000-entry cap, auto-scroll, level colors).

**Draconic does it with ONE object instead of three**, because core already has a global `Logger`
with pluggable `ILogSink`s that the whole engine logs through (`DRACONIC_LOG_*`):
- **`EditorLogBuffer : ILogSink`** (`editor.core`, headless) - thread-safe BOUNDED ring
 of full-fidelity entries `{level, category, message, sequence}` (heap strings - core's
 `RingLogSink` truncates messages to 192 chars, useless for build errors/paths). Registered on
 `GlobalLogger()` first thing in `main`, so early startup logs are captured (Sedulous's goal,
 without its unbounded growth - the cap + monotonic sequences let the UI report "N dropped").
 Consumers poll `CollectSince(sequence, out)` per frame on the main thread.
- **`LogView`** (`editor.app`) - Sedulous's LogView shape on foundation.ui: level-colored
 rows, per-level filter toggles + Clear, entry cap, auto-scroll, plus CATEGORY display (core
 logs carry categories; Sedulous had none). Lives in the Console panel;
 `EditorApplication::OnUpdate` drains buffer → view once per frame.
- Console/stdout output stays the core console sink's job (added alongside in `main`) - the
 editor never replaces the logger, it just adds sinks. Engine-wide capture is free.

## 4. Dependencies / sequencing

- ~~Content-DB text format~~ ✅ DONE (XmlSerializerFactory).
- ~~Scene serialization~~ ✅ DONE (SerializeScene / SaveScene - found during this survey).
- **Offline cooker driver** (registry, routing, incremental) - needed by the asset browser's
 import/build; phase 6 (§7).
- **Prefabs** (+ model→prefab) - needed by the spawn workflow; sequencing = §5b.

## 5. Decisions

**Already decided:**
- UI toolkit = **foundation.ui** (user, 2026-07-10) - docking via toolkit DockManager/DockablePanel.
- Editor architecture = §3 (context + registries + pages; statically-assembled plugin modules).
- Undo = **per-page IEditorCommand stack with merge + groups** + serialize-snapshot fallback for
 asset objects. (UI `UndoStack` stays a text-editing detail inside controls, not the editor stack.)
- Inspector = **reflection-first grid + attribute channel + InspectSerializer fallback +
 per-type overrides**.

**(a) Scene edit-time data model - ✅ DECIDED (user, 2026-07-11) = live-scene editing** (Lumix/Sedulous):
edit the live ECS through commands, snapshot/restore for play-in-editor.
*Why:* Draconic scenes already serialize the live world - `SaveScene` exists and "the cooked file
IS the authored form"; there is no `EntityData`-style source object model to project from.
Traktor's cook-from-source alternative (authoritative serialized `EntityData`, runtime entities a
disposable projection rebuilt per edit) is architecturally cleaner for undo (snapshot the data,
rebuild) but would mean inventing a parallel scene source format + adapter/rebuild machinery and
abandoning the working SerializeScene path. Assets (textures/models/materials) keep the
Traktor-style source→cook they already have - this hybrid is exactly what Sedulous ships.
*Cost of the live model:* undo correctness depends on ALL mutations routing through commands
(§3.4 rule), and destroy-undo must restore full serialized entity state (Sedulous left this a
lossy TODO - we won't).

**(b) Prefabs: build-first vs bootstrap-and-backfill - ✅ DECIDED (user, 2026-07-11) =
bootstrap the editor, backfill prefabs at the spawn milestone** (phase 7, after asset browser +
cooker).
*Why:* phases 1-5 (shell, viewport, hierarchy, selection, inspector, gizmos) don't touch prefabs;
designing the prefab system after the editor's spawn/override workflows are concrete avoids
building it blind (Lumix `PrefabSystem` + Sedulous `LocalModifications`/`PrefabRebuilder` both
show prefab design is dominated by editor use-cases: instance tracking, per-instance overrides,
apply/revert). The cooker driver lands with the asset browser (phase 6) for the same reason.

**The UI thread's three rules for asset work** (2026-09-23, after the RTHomes1 grid-view freeze
and the Bistro import stall): (1) a thumbnail generator's `Prepare` OPENS its source and composes
a header; the light worker reads the stream (`ThumbnailPrepared`) - Prepare used to read whole 4k
textures on the main thread, eight per `Get` burst. (2) An importer's `Import` is the DB fan-out
only: the CPU bulk (a mesh's conversion, LOD chain and serialization) rides the deferred writes
as a LAZY `produce` step the worker runs before the write, while the prepared model the editor's
job captures keeps the source alive. (3) A probe reads a header, never a file:
`image::dds::ReadDdsHeader` gives the texture setup its facts from 148 bytes. The assets view
logs each import's main-thread milliseconds and its worker flush; the MCP `asset_import` tool
reports `prepareMs` / `mainMs` / `flushMs` over the same two-phase path (Bistro, 1149 assets:
main-thread 16.3 s -> 0.9 s).

**Rule four: a settle-reload never block-completes** (2026-09-23, the Bistro prefab open). The
scene page binds under an `AsyncBindScope` and the 1071 cooked products (2.1 GB) decode on the
job system's workers, but when the first texture of a material settled, the reload of that
material (the recorded dependency edge) ran in the caller's SYNC mode: its other, still pending
textures were block-completed, and a sync bind of a pending id finalized EVERYTHING decoded so
far - nested, so the outermost finalize held the UI thread for 8 s (measured with the new
`finalize of <type> <id> took N ms` line: 148 finalizes past 50 ms, the tail climbing from 0.5 s
to 8 s as the stack unwound). Now the dependents' reload runs with async binds on (pending
slots stay skipped and each settle reloads again), and a sync upgrade of a pending id finalizes
that id alone; the rest keep their FIFO turn in `Pump`. The console reports each burst once:
`Resource: async loads settled: N in flight at the peak, M ms`; the page logs `opened scene
'x': parse / bind / prefabs ms (UI thread)` and the context logs each page's open time (the
Bistro prefab: 2.9 s, of which the 49 MB XML parse is 2.7 s). The binary serializer moves
scalar arrays (vertex bytes, index runs) as one block instead of per element. In Debug a
finalize can still show 50-100 ms for a shader compile or a pipeline creation under
validation - expected, not a stall to chase. Open follow-up: the XML parse is the remaining
UI-thread cost of a large source scene (parse on a worker, or a binary source form).

**DEFERRED (tagged 2026-07-11, plan/discuss later):**
- **Project native module**: optional per-project native DLL (game code + its editor plugins)
 loaded by the editor, with a static-link build option like Traktor. Touches: manifest field,
 module ABI/entry point (`RegisterEditor` C-linkage export), engine DLL-boundary story (currently
 all-static), hot-reload ambitions, play-in-editor with game subsystems. See §3.9 note.

## 6. Survey - condensed findings (2026-07-11, three parallel agents; full reports in session)

### Traktor (gold standard) - `/home/robert/Dev/CPP/traktor/code/Editor`
- `EditorForm : ui::Form, IEditor` - the shell window IS the service object pages receive.
 Own retained UI lib; `ui::Dock` pane tree (west/east/south tool panes + center `MultiSplitter`
 of `ui::Tab` groups for documents; side-by-side via tab-group split).
- **All extensibility via RTTI discovery**: `type_of<I>().findAllOf()` + `createInstance()` - 
 subclass + link = registered. Interfaces: `IEditorPageFactory`/`IEditorPage` (document tabs),
 `IObjectEditorFactory`/`IObjectEditor` (modal-less dialog editors), `IEditorPlugin` (windowless,
 ordinal-sorted), `IEditorTool` (Tools menu), `IWizardTool` (db context menu; import lives here).
 Dispatch = nearest-type match (`type_difference` min) across page + object-editor factories.
- **Two dbs** (source authored / output cooked), both open; editing = instance checkout →
 getObject → mutate → setObject → commit. Editor runs a remote-db server; the game connects as a
 client of the output db → content delivery + hot reload = output-db commit events.
- **Pipeline**: per-type `IPipeline`, 2-phase (parallel dependency walk → build), incremental via
 global inclusive hash vs pipeline-db, background build thread; auto-build on file change
 (asset monitor) and on source commit; `needOutputResources` forces build-before-open.
- **Inspector = the serializer**: `InspectReflector : Serializer` builds the property grid from
 the object's own `serialize()`; `ApplyReflector` writes back. Attributes on members refine
 widgets. `AutoPropertyList::bind(object)`.
- **Undo = whole-document `DeepClone` snapshots** (`Document::push()` before mutation, e.g. at
 gizmo `PreModifyEvent`); modified-detection via `DeepHash`.
- **Scene editing = cook-from-source**: `EntityAdapter` maps source `EntityData` ↔ built runtime
 `Entity`; `buildEntities()` rebuilds the runtime graph from data on essentially every edit
 (guid-keyed adapter cache + component-product hashes make it incremental; `trivialChange` path
 for scrubbing). Runtime entities are a disposable projection.
- Picking = per-adapter `IEntityEditor::queryRay`, nearest hit; modifiers (`IModifier`) =
 translate/rotate/scale with hover/begin/apply/end protocol + `TransformChain`.

### Lumix - `/home/robert/Dev/CPP/LumixEngine/src/editor`
- `StudioApp` shell on ImGui dockspace; plugin interfaces: `IPlugin` (subsystem editor, dependency-
 sorted `init()`), `GUIPlugin` (dockable window/frame hook), `MousePlugin` (viewport interaction),
 `IAddComponentPlugin` (Add Component menu tree), + `AssetBrowser::IPlugin`,
 `AssetCompiler::IPlugin`, `PropertyGrid::IPlugin`. Subsystem entry = `LUMIX_STUDIO_ENTRY`;
 its `init()` registers everything imperatively.
- **Edits the LIVE World**; `WorldEditor` mediates - every mutating method constructs an
 `IEditorCommand` (`execute/undo/getType/merge`); linear stack + index; same-type top merge
 (slider drag = one entry); `begin/endCommandGroup` transactions with same-type coalescing +
 explicit lock; failed execute → dropped. Selection/camera not undoable.
- **PropertyGrid** = reflection visitor (`IPropertyVisitor` per-type `visit`), attributes
 (radians/min/clamp/resource/color/enum/no-ui), multi-entity edit, per-component plugin UI after
 the reflected pass; all writes via `editor.setProperty` (commands).
- **Gizmo** = immediate-mode, global state, `manipulate(id, view, transform&)`; commits via
 WorldEditor calls over a topmost-only selection filter. Picking = CPU raycast (renderer
 ray-vs-mesh) + icon raycast; rect-select GPU-assisted.
- **AssetCompiler** = load-hook lazy compile: engine load → stale check (timestamps vs `.meta` +
 source) → background job queue (fiber) → content-addressed `.res` output (LZ4) → hook continue +
 live `reload()`; FileSystemWatcher feeds change queue; non-resource deps (shader includes)
 tracked + dependents recompiled; per-path generation counter kills stale jobs.
- **Play-in-editor** = serialize world to memory → run same live world (`startGame`) → on stop pop
 play-mode commands, destroy world, recreate, deserialize snapshot; undo disabled across the
 boundary; deferred exit at frame top.

### Sedulous - `/home/robert/Dev/Beef/SedulousEngine/Code/Editor`
- **Full mature editor** (~90 files): `Sedulous.Editor.Core` (headless domain) + `Sedulous.Editor`
 (UI) + thin `.App`. Shell = MenuBar + toolkit DockManager + StatusBar; `EditorDockHost :
 IDockableWindowHost` (floating OS windows - we already ported this pattern);
 layout persistence to `editor_layout.oddl`; project manager (`.sedproj`).
- **`EditorContext` service locator** + registries: `IEditorPageFactory` (~14 asset-type pages,
 each page owns id/title/dirty/**own CommandStack**), `IEditorPanelFactory`, `IAssetImporter`
 (CreatePreview → configure dialog → Import; **import IS the bake**, no cooker),
 `IAssetCreator`, `IThumbnailGenerator`, `IGizmoRenderer`.
- **Embedded runtime context** (full engine subsystems at edit time; game module's hooks run
 against it; fallback subsystems for asset-only editing). Live scene editing; prefabs first-class
 (`PrefabSpawner/PrefabRebuilder/LocalModifications` per-instance overrides).
- Inspector = comptime-generated `DescribeProperties` from `[Property]` attrs
 (two-layer: headless describe / widget building); viewport = `ViewportView` + `ISceneRenderer`
 into offscreen + priority input handlers (gizmo first, then fly-cam); **GPU picking**
 (`PickPass.RequestPick`) with CPU ray fallback; `TransformGizmo` drag-end pushes
 `SetTransformCommand`; play-in-editor via `SceneSnapshot.Capture/Restore`.
- **Weaknesses (our fixes in §3):** `[EditorPlugin]` discovery fully built but **zero users** - 
 everything hard-registered in `EditorApplication.OnStartup` (1424-line god-class) so the editor
 links every engine module → fixed by §3.1 assembly model. **Undo coverage shallow** - only
 create/destroy/transform are commands; inspector edits/rename/reparent/Add-Component mutate
 directly; `DestroyEntityCommand.Undo` is lossy (TODO) → fixed by §3.4 rule. Dead scaffolding
 (`EditorSceneManager` stubs); hierarchy+inspector hardwired in an 870-line static builder →
 ours are standalone reusable panels.

## 7. Phased build plan (§5a=live, §5b=bootstrap - both confirmed 2026-07-11)

1. **Shell** - `editor.core` (EditorContext, registries, `EditorCommandStack` +
 `IEditorCommand` + merge/groups, selection model, `EditorPage` abstraction; doctests for all
 of it) + `editor.app` + editor executable: main window, MenuBar/StatusBar,
 DockManager with the GLOBAL panels only (center document area w/ Welcome placeholder, Console
 bottom, Assets tabbed with Console), floating via RuntimeDockableWindowHost, dock-layout
 persistence, project = open source DB (XML factory).
2. **Scene page + viewport** - scene-page factory; page docks center as a closable tab; each page
 owns its OWN live Scene (multi-scene) rendered via foundation.render into its `ViewportView`;
 per-page `EditorCamera` fly on the gated InputSurface; open/save scene through the content DB
 (`SaveScene`).
3. **Hierarchy + selection** - entity tree as a standalone reusable view INSTANTIATED INSIDE the
 scene page (per-page selection set + events), create/delete/rename/reparent as commands
 (destroy-undo restores full serialized state), viewport CPU-ray picking.
4. **Inspector** - DONE. reflection grid as a standalone reusable view inside the scene page
 (asset pages reuse it too) + `SetComponentProperty`/`Raw` commands w/ merge + Add/Remove
 Component (from the scene's component-manager registry, full-state undo). Deferred from the
 original sketch: attribute channel, `InspectSerializer` fallback, `IPropertyEditor` overrides
 (revisit when asset pages need the grid); resource-ref pickers land with phase 6.
5. **Gizmos** - DONE (design in §8). TransformGizmo on debug-draw (translate/rotate/scale,
 world/local, snap), command-group bracketing (one undo entry per drag), per-component
 `IGizmoRenderer` (light/probe/camera built-ins, selected-only by default).
6. **Asset browser + cooker** - cooker driver library (builder registry + routing + incremental
 hash + dep order; CLI tool + in-editor background build), browser panel (tree + grid +
 thumbnails), import via `IAssetImporter` (model/texture first), open-in-page dispatch, cooked
 hot reload.
7. **Prefabs** - prefab resource + spawn + model→prefab import workflow (+ per-instance overrides
 as a follow-on).
8. **Play-in-editor** - snapshot → run → restore; input capture handoff; undo fence.

Each phase lands with doctests (core logic headless in `editor.core` / `foundation.<sys>.editor`),
verified on DEBUG clang + gcc.

## 8. Gizmos design (phase 5, LOCKED 2026-07-11)

**Sources:** Sedulous TransformGizmo/GizmoInputHandler (skeleton - fits our debug-draw +
command-stack + InputSurface architecture) **improved with PlayCanvas** (src/extras/gizmo survey,
2026-07-11). Not a blind port; the deltas below are deliberate.

**Handle set** (all proportions × screen-constant Size):
- Translate: 3 axis arrows + 3 camera-facing-quadrant plane quads + center free-move (camera
 plane). PlayCanvas addition: planes + center; Sedulous had axes only.
- Rotate: 3 axis rings (back half culled - only the camera-facing half drawn/pickable; full ring
 for the active axis while dragging) + outer screen-space ring (normal = camera forward).
- Scale: 3 box-tipped axes + center uniform-scale handle. **Scale always operates in Local space**
 (PlayCanvas rule - world-space non-uniform scale on a rotated entity is skew).

**Picking:** analytic (ray-vs-segment / ray-vs-ring-band / ray-plane-quad - we render debug-draw
ribbons, not meshes, so no tri-mesh pick) with the PlayCanvas **priority system**: center (2) >
planes / screen ring (1) > axes / rings (0); priority beats distance. Pick zones are inflated vs
the rendered thickness. **Grazing-angle handling** (PlayCanvas): axis handles disable (unpickable
+ drawn faded) when the axis points at the camera (1-|dot| < 0.01); plane quads disable when
edge-on; quads flip into the camera-facing quadrant.

**Drag math:** Sedulous core - drag plane contains the axis and is most perpendicular to the
view; delta projected on the axis. Rotate = atan2 on a basis captured at BeginDrag (plane can't
drift as the entity rotates) + seam unwrap into (-pi, pi], + PlayCanvas guards: behind-camera hit
retries the reversed ray, screen ring uses a camera-plane basis. Uniform scale projects the
camera-plane delta onto the screen diagonal. **Parent-aware** (improvement over Sedulous, which
ignored parents): world-space deltas convert into the entity's parent space (translate via
inverse parent world; rotate conjugated: local' = inv(pQ) * dq * pQ * localStart).

**Snap** (held Ctrl): PlayCanvas relative-delta quantization - round(delta/inc)*inc measured from
the drag start (not an absolute world grid). Defaults: translate 1.0, rotate 15 deg, scale 0.25;
public fields.

**Size:** PlayCanvas screen-constant scale = tan(fovY/2) * viewDepth * 0.3, where viewDepth =
dot(gizmoPos - camPos, camForward) (true view depth, not radial distance).

**Visual feedback:** hover = 75% lerp toward white; active axis = yellow; rotate shows an angle
guide (faint start-reference line + solid current line + degree readout via DrawText3D); axis
drags show a full-length span line (depth-tested solid + faint overlay through geometry); mode +
space shown as viewport screen text.

**Input & commands:** keys W/E/R = translate/rotate/scale, X = world/local toggle (gated: viewport
hovered, RMB up so fly-camera WASD wins while flying). Drag session = BeginGroup("gizmo_drag") ->
per-frame SceneEditContext::SetLocalTransform (merges inside the group into one command) ->
EndGroup + LockGroup on release. The group markers stop cross-gesture merging (a drag can never
merge with a prior inspector scrub) and the lock stops consecutive drags coalescing: exactly one
undo entry per drag, restoring the exact start transform. The gizmo consumes LMB when a handle is
hot; PickOnClick runs otherwise.

**Structure:** `editor.scene:gizmo` - TransformGizmo (pure math + debug-draw rendering;
headless-testable) + GizmoController (drag session; driven by a plain GizmoFrameInput struct so
tests can script full drag sessions without a shell). Component gizmos: IGizmoRenderer +
GizmoContext + registry in the same module (the scene-editor plugin already links the render
subsystem; editor core stays engine-free). Ports-with-fixes: LightGizmoRenderer (directional sun
cross+arrow / point range sphere / spot cone from range+outerAngle), ReflectionProbeGizmoRenderer
(wire BOX from halfExtents - our probes are boxes, Sedulous drew a sphere). Default
DrawWhenUnselected=false (Sedulous drew every light's range sphere always - noisy; markers
already anchor unselected entities).


## 9. Headless flags (Tools.Editor)

`Tools.Editor [<projectDir>] [--project <dir>] [--exit-after <s>] [--rebuild-after <s>]
[--seed] [--seed-primitives] [--data-root <dir>] [--vulkan|--webgpu|--dx12]`. A bare directory
scaffolds a fresh project (the manager's New Project flow seeds starter content; a CLI scaffold
does not, unless `--seed` = font/sky/three primitives, or `--seed-primitives` = the same plus
every primitive creator). `--seed-primitives` + `--exit-after` is the headless way to regenerate
primitive meshes at the current source version (paperkid.md, upgrade recipe).

`--screenshot <png> [--screenshot-after <s>]` (2026-09-20) writes the MAIN window's backbuffer, UI
included, as a PNG once that many seconds have run - the same `ScreenshotCapture` the runtime's
`--screenshot` uses, recorded after the UI host's draw. With `--exit-after` it is a headless proof
of what the editor drew: `Tools.Editor <project> --seed --screenshot shot.png --screenshot-after 6
--exit-after 9`. Ported from the Beef side, which also found that a viewport laid out before its
device arrived kept its placeholder forever (fixed: `ViewportView::Initialize` makes the targets
for a view that already has a size).

