# Model viewer: a standalone tool to look at and pick models

Status: proposed 2026-10-29. Approved to build now as a standalone tool (user 2026-10-29).

## Why

Linux has no decent model viewer, and looking at a model in the engine means importing it into a
project first. Picking from a large collection (a store pack, a folder of hundreds of FBX files)
needs a viewer that opens anything from disk at once, plays its animations, and browses a whole
folder as thumbnails. Legacy Sedulous had one (`origin/v0`, `Code/Tools/Sedulous.Tools.ModelViewer`:
tabs, an info panel, an animation bar, an orbit/fly camera); this recreates it on the engine and
the new GUI, and adds the browsing and picking a large collection needs. It may later ship as Linux
software of its own.

## What it is

`Tools.ModelViewer` (`Code/Tools/Tools.ModelViewer`), a windowed app on the engine runtime and the
GUI, beside `Tools.Editor`. It never needs a project.

- **Open**: files on the command line, dropped on the window, or from the native Open dialog
  (glTF, GLB, FBX, OBJ). Each model opens in a closable tab.
- **Look**: a lit scene (a sun, the procedural sky, a ground grid), the editor's camera (orbit,
  fly, pan, zoom; F frames the model), exposure, ambient and sky intensity, a turntable, a skeleton
  overlay, a screenshot button.
- **Know**: the side panel shows the file, meshes, vertices, triangles, materials, textures,
  bones, clips and size.
- **Animate**: a clip list, play/pause, stop, step, loop, speed and a scrub bar.
- **Browse**: a folder pane (a tree of folders) and a thumbnail grid of every model under the
  chosen folder, rendered by the engine, cached on disk, and filtered by name, rigged and animated.
  Clicking a thumbnail opens it.
- **Pick**: star a model and give it a note; the shortlist is saved beside the viewer's settings
  and exports as CSV.
- **Loose textures**: a model whose material lost its texture on export (common with FBX files
  from packs and extractions) gets the PNG beside it that shares its name, or the first one, so it
  shows painted, not grey. On by default; a toggle turns it off.

## How it is built (what exists)

- **Loading** (no project): `model::io::loadModel` with the glTF and FBX loaders (ufbx reads FBX
  and OBJ) gives the in-memory `model::Model` (stats come from it); `pipeline::CookModel` cooks it
  into a content database; `ResourceManager::Bind<ModelResource>` gives the meshes, materials,
  skeleton and clips. The database lives in a cache folder of the viewer's (under the user data
  folder, `model-viewer/cache/<hash of path, size and mtime>`), with a record of the model's guid
  and its numbers beside the cooked files, so a model opened again skips the load and the cook. Samples/Sandbox does this whole path today (`LoadImportedModel`, `SpawnModel`).
- **Window, GUI, scene**: `runtime::RunApplication` with an `IApplication`; the GUI host
  (`gui::GuiHost`) as GUISandbox sets it up; an embedded engine runtime (`DefaultApplication` on
  an `EmbeddedApplicationHost`) as the editor's `StartEmbeddedRuntime` does.
- **Viewport and camera**: `editor::PreviewViewport` (a `gui::Viewport` with its own scene, the
  editor camera, frame and debug draw) and `editor::EditorCamera` (header only: Core and Shell).
  PreviewViewport depended on Editor::Core only for its allocator, but shared a library with the
  thumbnail stage, which needs the core: it moves to a library of its own
  (`Editor::Preview.Viewport`, module `editor.preview.viewport`, re-exported by `editor.preview` so
  the pages are unchanged) and takes its allocator as a parameter. The tool links it and
  `Editor::Camera`, and nothing else of the editor.
- **Spawning**: one entity per model node, a mesh component per mesh with its materials, a
  skeletal animation component on a rigged model: lifted from Sandbox's `SpawnModel` into
  `engine::model::InstantiateModel` (`Engine::Model`), which the viewer and Sandbox share.
- **Overlays**: `debug::DebugDraw` (grid, lines); the skeleton drawn as the animation page does
  (`DrawSkeletonBones`, about 20 lines over `Skeleton::ComputeWorldPoses`).
- **Animation**: `SkeletalAnimationComponent` and its `AnimationPlayer` (Play, Pause, Resume,
  Stop, SetCurrentTime, speed); a paused scrub samples with `animation::SampleClip`.
- **Thumbnails**: an offscreen render and readback as `editor::ThumbnailStage` does it (rendered
  at 4x and box filtered), keyed by path and mtime instead of a content guid.
- **OS**: drops through `GuiHost::SetDropObserver`; dialogs through `IShell::Dialogs()`
  (`ShowOpenFile`, `ShowOpenFolder`).

New: the cache folder, the path-keyed thumbnail service, the folder pane and grid (built from the
GUI's TreeView, ListView, Image and the Toolkit's BreadcrumbBar), the shortlist store, the loose
texture rule, and the tool's own stylesheet and fonts (the editor's sheets stay the editor's).

Out of scope for now: editing models, exporting other formats, more than one skin per model (the
cook takes skin 0; noted where the side panel says so), materials beyond the engine's PBR import.

## Phases (one commit each, with tests)

- **P0 Skeleton.** The tool target, its window, GUI and embedded runtime; a model from the command
  line in one viewport, lit, with the grid and the camera; the spawn function shared with Sandbox.
  Tests: the load and cook into a cache folder (a glTF from Data/Tests) and its cache hit; the
  spawn's entities and components. `--screenshot <png>` as the player has, for visual checks.
- **P1 Tabs and panel.** Tabs, drag and drop, the Open dialog, the side panel's stats, exposure,
  ambient, turntable, screenshot. Tests: the stats from a known model; tab open and close.
- **P2 Animation.** The animation bar and the skeleton overlay. Tests: clip selection, pause and
  scrub times, loop.
- **P3 Browse.** The folder pane, the thumbnail service and grid, the filters. Tests: the folder
  scan and filters; thumbnail cache keys and invalidation on a changed file.
- **P4 Pick.** Stars, notes, the shortlist and its CSV; the loose texture rule. Tests: the store's
  round trip; the texture rule on a model without texture links.
- **P5 Ship.** A Linux distribution script (as the editor's), a README, the user guide page, and
  screenshots for the user's look.

Each phase is checked by the user on screen; the tool's own `--screenshot` gives pictures to hand
over.
