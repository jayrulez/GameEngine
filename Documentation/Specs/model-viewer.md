# Model viewer: a standalone tool to look at and pick models

Status: approved to build now as a standalone tool (user 2026-10-29). P0-P5 built 2026-10-29
(committed; P1-P5 await the user's check). Phase two (P6-P11, the library and its MCP)
proposed 2026-10-29.

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
  Built 2026-10-29: `modelviewer::ModelTabs` (a file opened again goes to its tab; closing hands
  the front to the neighbour), each tab's instance kept in the one preview scene and active only
  while in front, its camera remembered; the panel (the inspector's rows) with the size measured
  as placed; exposure, ambient and sky sliders; the turntable about the footprint's middle; the
  screenshot is the view only, through a save dialog. The counts were fixed on the way: triangles
  of a mesh with no index buffer, bones as the animated skin's joints (not every node).
- **P2 Animation.** The animation bar and the skeleton overlay. Tests: clip selection, pause and
  scrub times, loop. Built 2026-10-29: `modelviewer::ClipPlayback` over the instance's animator
  (a choice plays from its start on the next update; stop is the start, paused; a step is a 30th
  of a second; the viewer's copy of a clip loops by default, the file untouched); the bar under the
  view (clip, play/pause, stop, step back and forward, scrub with the time, loop, speed), hidden
  for a model without clips; the bones drawn over the model in the pose it stands in
  (`--skeleton` starts with them on).
- **P3 Browse.** The folder pane, the thumbnail service and grid, the filters. Tests: the folder
  scan and filters; thumbnail cache keys and invalidation on a changed file. Built 2026-10-29: a
  Browse tab, always first, in place of the stage while in front: a folder tree (filled as folders
  are chosen, a level ahead), the filters (name; rigged and animated, known once a model is drawn,
  from its cache record) and a wrapping grid of every model under the chosen folder; a click opens
  it in a tab. The folder is kept in `model-viewer/settings.txt`. Thumbnails are the viewer's own
  renderer (`modelviewer::ThumbnailRenderer`, the window's half): one model at a time in a private
  scene in its rest pose, 128 pixels from the front quarter, read back to a PNG under
  `model-viewer/thumbnails`, named by the file (`ThumbnailPathFor`). A model cooks on the main
  thread, so the window stalls a moment per model while a folder is first drawn (about four a
  second on the Juvi folder); a worker for the cook is the next step if that is too slow.
- **P4 Pick.** Stars, notes, the shortlist and its CSV; the loose texture rule. Tests: the store's
  round trip; the texture rule on a model without texture links. Built 2026-10-29:
  `modelviewer::Shortlist` (a note stars a model; kept in `model-viewer/shortlist.txt`, a line a
  model with tabs and line breaks escaped; CSV of name, path and note, quoted where needed); the
  panel's Pick section (the star and the note of the tab in front), the browser's Starred filter
  (a starred tile's name in the accent) and Export CSV. `ApplyLooseTextures` runs between the load
  and the cook: a material with no colour texture takes the PNG or JPEG beside the model named
  like the material, else like the model, else the first in name order. The toggle is in the View
  section, kept in the settings, for models opened from then on; it is part of the cache key, so
  each choice cooks once.
- **P5 Ship.** A Linux distribution script (as the editor's), a README, the user guide page, and
  screenshots for the user's look. Built 2026-10-29: `scripts/build-model-viewer-dist.sh` (the
  clang-reldbg build, the exe and its sidecars, Data/Engine, Data/Editor without its mock,
  Data/ModelViewer, the cooked shader pack, a README.txt; the symbols split off) makes
  `dist/ModelViewer-<version>-Linux64.tar.gz` (27 MB); unpacked outside the checkout it runs. The
  user guide page is `tools/model-viewer` in the docs repo, under a new Tools section.

Each phase is checked by the user on screen; the tool's own `--screenshot` gives pictures to hand
over.

## Phase two: the library (proposed 2026-10-29)

P0 to P5 made a viewer: it looks at files and picks from a folder. Phase two makes it the place a
game's models are organised: a library of the models you keep, in collections that nest, with
tags, notes and the names a game knows them by, searched across, and gathered into a folder for a
project. And the polish the first phase lacked: no stalls while a folder is drawn, preferences,
recents, multi-select, a menu bar. Decided with the user 2026-10-29: the shortlist becomes a
collection; collections nest; a name is unique across a top-level collection; the library is XML
in the user data folder; every operation is a command, so an agent can work the library through
MCP as the user does through the window.

### The library

- **An entry** is a model you added: a stable id (a guid), its **path**, the file's size and time
  when added (so a moved or changed file is noticed and can be relinked to the file now at that
  place, or to one the user points at), its **tags**, a **note**, and its numbers as the last cook
  counted them (rigged, animated, clips, triangles, size), so search never has to cook.
- **A collection** has a name, a parent (none for a top-level collection) and its members, in
  order. A member is an entry and the **name** it has in that collection (`knight_red`, the id a
  game would use; empty shows the file's name). An entry may be a member of any number of
  collections at any level. A name is unique across a top-level collection and everything under it
  (a game's acts share one set of ids; two games may each have a `hero`); a rename that would clash
  is refused, naming the member that holds it.
- **Deleting** a collection takes its sub-collections and memberships with it, never an entry; an
  entry no collection holds stays in the library (the **Unsorted** view lists them) until the user
  removes it. Removing an entry removes its memberships.
- **The shortlist becomes a collection**: on the first run of phase two, `shortlist.txt` is read
  into a top-level collection named Shortlist (a member per starred model, its note the entry's
  note), and the file is renamed `shortlist.txt.imported`. The panel's Shortlist toggle is gone:
  collections take its place.
- **Stored** as `model-viewer/library.xml` (the engine's XML serialization, with a data version),
  written after each change through a temporary file and a rename, so a crash never leaves half a
  library. A library of some thousands of entries stays small.

### Commands, and MCP

- **Every operation is a command**: a name (`collection.create`), its arguments as JSON with a
  schema, its result as JSON, and whether it only reads or changes something. One dispatcher runs
  them. The window builds its calls from commands (a button, a menu item, a drop, a shortcut: a
  command with the arguments the window's state gives), and an agent calls the same commands
  through MCP. Nothing is done only from the window: a missing command for something the window
  does is a bug.
- **Changes are announced**: each command that changes the library or the window says what
  changed, and the window's views refresh from that, whoever ran it, so an agent's edits appear as
  they happen.
- **Undo**: each change to the library records its inverse; Ctrl+Z (Edit > Undo) and the
  `history.undo` command step back through them, an agent's and the user's alike, and
  `history.list` says what they were.
- **The commands**, in groups:
  - `entry.*`: add (a path; its entry when it has one), remove, relink, get, note.set;
  - `collection.*`: create (a name and a parent), rename, move, delete, tree, members (with or
    without what is under it);
  - `member.*`: add (entries to a collection, at a place), remove, rename (the name a game knows
    it by; a clash refused, saying who holds it), move;
  - `tag.*`: add and remove (on entries), rename and delete (everywhere), list (with counts);
  - `search` (the query language above, a scope, a limit), `export.csv`, `export.copy_files`;
  - the window's: `model.open`, `model.close`, `tabs.list`, `tabs.select`, `view.screenshot` (a
    path, or the screenshot folder; returns the file), `view.camera` (frame, or a place to look
    from), `view.light`, `clip.select`, `clip.play`, `clip.scrub`, `browse.folder`,
    `thumbnail.get` (a model's thumbnail file, drawn if needed);
  - `settings.get` and `settings.set`, `recents.list`, `history.undo`, `history.list`.
- **The MCP host**: `--mcp` (or `--mcp-port <port>`) serves the commands over HTTP
  (foundation.mcp.http: localhost and a bearer token, the editor's token file), each a tool named
  after its command (`collection_create`), with the schema and the read-only or destructive hint
  from the command, and a `docs://ModelViewerGuide.md` resource, the agent's manual
  (Documentation/Shipping/ModelViewerGuide.md). `--headless` serves them over stdio with no window,
  for an agent organising a library from a terminal: the library, search and export commands, and
  thumbnails drawn offscreen; the window's commands answer that there is no window.
- **One writer**: a running viewer holds a lock beside the library; a second one (a window or
  headless) opens it read-only and says so, pointing an agent at the running viewer's MCP port.

### The Library tab

A tab beside Browse, both always first.

- **Left**: the collection tree (dragged to re-parent; right-click to add a sub-collection, rename,
  delete), then **All models** and **Unsorted**, then the tags with their counts.
- **Right**: the grid, the browser's tiles: the selected collection's members (with **Include
  sub-collections**, on by default), all entries, the unsorted ones, or those with a tag. A member's
  tile shows its name over its file's name. A missing file's tile says so and offers **Relink...**.
- **Search**, one box: plain words match a member's name, the file's name, the note and the tags,
  any case; `tag:boss`, `collection:act1` (with what is under it), `rigged`, `animated`, `missing`
  narrow it; terms combine with and. It searches the selected collection (and its sub-collections)
  or, with **Everywhere**, the whole library.

### Adding and editing

- **From the browser**: select tiles (click, Ctrl+click, Shift+click, Ctrl+A) and **Add to
  collection** (a picker of the tree, or a new one), or drag them onto a collection in the Library
  tab's tree. Adding a model makes its entry if it has none.
- **From an open tab** (a browsed model or any file opened on its own): the side panel's
  **Library** section shows the collections it is in and its tags as chips, each removable, with
  **Add to collection** and a tag field that suggests existing tags; its note; and, per collection,
  its name there (edited in place, a clash said at once).
- **Tags** are free text, case kept, matched any case; renaming or deleting a tag in the tree does
  it on every entry.

### Exports

- A collection's **CSV** (with or without what is under it): collection path, name, file, tags,
  note.
- **Copy files to folder...**: a collection's model files, and the images beside each that its
  materials name (or the loose texture rule would take), into a folder, a subfolder per
  sub-collection, a file renamed to its member's name when it has one (its extension kept); a
  report of what was copied and what was missing. For bringing a game's models into its project.

### Preferences and recents

- **Preferences**, a page from the menu: the screenshot folder (default the user's Pictures folder
  under ModelViewer; or ask each time) and its file name pattern; the thumbnail size (96, 128,
  192); the cache folder, its size and **Clear cache**; the defaults for loose textures, loop,
  turntable speed, the light (exposure, ambient, sky) and the grid; the interface density (the
  editor's densities); how many recent items to keep. Kept in `model-viewer/settings.txt` as now
  (its lines, an unknown one kept).
- **Recents**: the models opened and the folders browsed, newest first; in **File > Open Recent**
  and on the Browse tab when no folder is chosen.

### Polish

- **The cook on a worker thread**: a model loads and cooks off the main thread (the content
  database and the resource binding stay on it, as the editor's import does), so the window never
  stalls; a progress line under the grid ("drawing 37 of 1,106"), and opening a model shows its tab
  at once with "cooking" over the view until it is ready.
- **A menu bar**: File (Open, Open Recent, Close Tab, Screenshot, Preferences, Quit), View (the
  panel, the skeleton, the turntable, the grid), Library (new collection, add to collection,
  export, copy files).
- **Shortcuts**: Ctrl+O open, Ctrl+W close the tab, Ctrl+Tab and Ctrl+Shift+Tab the next and
  previous tab, Ctrl+F the search, Space play or pause, the arrows step a frame, F frame.
- **The grid**: a tile size slider, multi-select, a right-click menu (open, add to collection, show
  in folder, copy path); tabs show the model's thumbnail when it has one.

### Phases (one commit each, with tests)

- **P6 The library and its commands.** The store (entries, collections that nest, members and
  their names, tags, notes), its XML round trip and data version, the atomic write, the lock, the
  shortlist's import, relink; the command dispatcher, the change announcements, the undo log, and
  the library's commands (`entry`, `collection`, `member`, `tag`, `search`, `history`). Tests,
  through the commands: the round trip; names unique per top-level collection (a clash refused,
  two games each with one); deleting a collection keeps its entries; the search's terms and
  scopes; undo of each change; the shortlist imported once; a moved file noticed and relinked; a
  second opener read-only. Built 2026-10-29: `modelviewer::Library` (`:store`), `Search`
  (`:search`), `CommandSet` and `LibrarySession` with `RegisterLibraryCommands` (`:commands`, 23
  commands); undo keeps the library as it was before each change (the last 50); names compare in
  any case; the lock is the system's (Core's FileTryLock, f6fa381d8), held while the library is
  open and released when the viewer ends, a crash included. The window does not use the library
  yet (P8).
- **P7 MCP.** The host over the dispatcher (`--mcp`, `--mcp-port`, `--headless` over stdio), the
  tools from the commands, the guide resource and its document. Tests: the tool list is the
  command list (names, schemas, hints) and the guide names every one; a call through MCP changes
  the library as the command does; a window command headless says there is no window. Built
  2026-10-29: `:mcp` (each command a tool, dots to underscores, its kind the annotations: reads,
  creates, changes, deletes, adjusts), `viewer.info` (its process, version, mode, library,
  writable), the guide `Documentation/Shipping/ModelViewerGuide.md` (staged beside the exe by the
  dist script), `--mcp` / `--mcp-port` (HTTP, 7406 by default beside the editor's 7405, the
  shared mcp-token) and `--headless` (stdio, logs to stderr). The window commands, and so the
  headless test of one, come with P8.
- **P8 The Library tab.** The tree, the views, the search box, adding from the browser
  (multi-select, the picker, a drop on the tree) and from the panel's Library section, tags and
  names edited, the window's commands (`model`, `tabs`, `view`, `clip`, `browse`, `thumbnail`);
  every control a command. Tests: adding a selection; renaming and deleting a tag across entries;
  an announced change refreshes the views it touches.
- **P9 Preferences and recents.** The page, the settings it keeps, the screenshot folder and
  pattern, the thumbnail size, Clear cache, the recents (`settings`, `recents`). Tests: the
  settings' round trip (an unknown line kept); recents' order and limit.
- **P10 Polish.** The worker-thread cook and the progress line, the menu bar, the shortcuts, the
  grid's size slider and right-click menu, thumbnails on tabs. Tests: a cook on the worker ends in
  the same cooked model as on the main thread; a queue cancelled mid-way leaves no half-written
  thumbnail.
- **P11 Exports.** A collection's CSV and Copy files to folder (`export.csv`,
  `export.copy_files`). Tests: the CSV's rows with and without sub-collections; the copy's layout,
  the renames, the images taken and the missing ones reported.

Out of scope here: sharing a library between machines or people (it is one user's file), and
bringing a collection into a project as assets (Copy files to folder hands the files over; the
editor imports them).
