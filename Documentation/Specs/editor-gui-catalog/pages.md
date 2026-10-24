# Asset editor page catalog (Code/Editor asset pages)

Scope: every asset editor page (and sub-piece) in Editor.Animation, Editor.Particles, Editor.PropertyAnimation,
Editor.Audio, Editor.Materials, Editor.Shaders, Editor.Script (+ .AngelScript, .Luau), Editor.GUI, Editor.GameUI,
Editor.Fonts, Editor.Texture, Editor.Image, Editor.Heightfield, Editor.Generic, Editor.Render, Editor.Geometry,
Editor.Physics, Editor.Input. Read-only survey of the code as of 2026-10-23.

All paths are relative to `Code/` unless absolute.

Conventions used below:
- "Asset type" is the reflected primary type the page factory registers (`PrimaryType()`). Every source asset
  instance is stored as a `.xasset` XML document in the project's Content tree (`typeName` names the type);
  cooked products are `.rasset`. Where the asset wraps an external source file, that file's extension is listed.
- "PageToolbar(Edit)" means `app::PageToolbar` with `Standard::Edit`: buttons Save, Undo, Redo, Discard Changes
  (action ids `file.save`, `edit.undo`, `edit.redo`, `page.discardChanges`). "PageToolbar(Save)" = Save only.
  "PageToolbar(None)" = no standard buttons. "+Playback" = `AddPlayback()`: Play (toggle, checked while
  playing; play/pause/resume), Stop (stop and rewind), Restart. Labels are text, no icons.
- "PreviewViewport" = `editor::PreviewViewport` (Editor.Preview), the shared 3D preview substrate.

---------------------------------------------------------------------------------------------------------------

## 0. Shared infrastructure every page builds on

### 0.1 Page model (Editor.Core/Page.cppm, Editor.App/UIPage.cppm)
- PURPOSE: one open asset = one dock tab ("center document area", closable) with its own undo stack, dirty flag,
  and Save. `EditorPage::DiscardChanges()` default undoes the whole stack. `OnAssetExternallyModified()` lets a page
  refresh after re-import; `OnSavedAs()` rebinds to a new instance.
- Services: `IPlaybackPage` (CanPlay/IsPlaying/Play/Pause/Stop/Restart) gives every playable page the same
  three toolbar buttons (Editor.App/PlaybackActions.cppm). `IGameRunPage` for Game tabs (not in scope).
- Dirty: `Commands().OnChanged` marks dirty; pages also call `MarkDirty()` directly. Tab title is `Title()`
  (instance name); the shell appends " *" to a dirty tab title and intercepts closing a dirty tab with a Save / Discard /
  Cancel dialog; exit confirms when any page is dirty. Shortcuts: Ctrl+S save (enabled only when dirty), Ctrl+Z undo,
  Ctrl+Shift+Z or Ctrl+Y redo; Discard Changes has no shortcut and no menu path; File > Save As... exists for every page.
- Factory dispatch is nearest-type along the base chain, so Editor.Generic's form page catches any type
  without a bespoke page.

### 0.2 PageToolbar (Editor.App/PageToolbar.cppm)
- Built from the action registry over THIS page (split layouts can show two pages). Buttons = action labels
  (text only). `Refresh()` per frame syncs enabled/checked. Pages may append raw `AddToggle`, `AddButton`,
  `AddLabel`, `AddItem` (arbitrary widget) after the bound actions. `PageToolbar::Frame()` stacks the bar
  over the content.
- PROBLEMS: text-only labels; no overflow handling for wide bars; pages that add raw toggles bypass the action
  registry (no shortcut, no command palette entry, no disabled reason); several pages do not use it at all (see
  the matrix in section 21).

### 0.3 PreviewViewport (Editor.Preview/PreviewViewport.cppm) + EditorCamera (Editor.Camera)
- A ViewportView + private preview Scene (simulation off by default) + EditorCamera + render loop. Pages
  populate the scene, call Update/RenderFrame, draw overlays via `SceneDebugDraw()`. `SetClearColor`,
  `SetSimulationEnabled`, `SetTimeScale`, `ResetScene`.
- Camera gestures (all 3D previews): RMB free-look + WASD/QE fly (Shift fast) while RMB held or Tab-captured,
  Alt+LMB turntable orbit about the focus pivot, MMB pan, wheel dolly toward pivot (exponential). Pointer lock
  during drags. No on-screen hint of these controls in any preview; no "frame object" (F) shortcut wired by the
  asset pages; no view-axis gizmo; no grid/background toggle exposed on asset pages.
- Each page sets its own clear colour and camera start pose (see per-page notes): inconsistent backdrops.

### 0.4 Toolkit widgets reused by pages (Code/Foundation/UI.Toolkit)
- PropertyGrid: categories (sections) with collapsible headers, `SetCategoryParent` (nested sections),
  `SetCategoryHeaderActions` (icons in a section header, e.g. remove). Editors: String, Float, Int, Bool, Enum,
  Float2/3/4, Color, Range, Button. A read-only stat is usually a StringEditor with an empty setter (looks
  editable, is not: recurring PROBLEM).
- SplitView: two panes, ratio, horizontal by default; `Orientation::Vertical` stacks. Nested SplitViews make
  three-pane layouts. Ratios are hard-coded per page and not persisted.
- DraggableTreeView / TreeView: virtualized rows, adapters, optional drag reorder, EditableLabel rows (inline
  rename via double-click / slow-click).
- NodeGraphCanvas: MMB pan, wheel zoom toward cursor, RMB context menu (node / connection / canvas), LMB select +
  drag + box-select, port drag to connect (drag from a connected input detaches and reroutes), double-click node,
  Delete key deletes selected, Ctrl+A select all, Esc cancels a pending link. `ConnectionStyle` Bezier (dataflow)
  or StraightNodeToNode (state machine with arrows). Nodes: title, subtitle, header colour, highlight ring.
- CurveCanvas: multi-channel Hermite keys; LMB select/drag key, tangent handles on the selected key, RMB on a key
  deletes (RMB on a handle switches mode), MMB pans values, wheel zooms the VALUE axis only, AutoFitValueRange.
- GradientEditor: colour/alpha stops on [0,1]; click strip adds a stop sampled from the ramp, drag moves,
  double-click a stop raises `OnStopColorRequested`, which NOTHING in the codebase handles (so an existing stop's
  colour cannot be changed anywhere), RMB removes.
- Timeline (dopesheet): ruler + playhead scrub, lanes with key diamonds, label gutter (click selects lane), box
  select, Ctrl multi-select, key drag (visual, one commit), MMB pan, Shift+wheel / horizontal wheel pan, wheel
  zoom anchored at cursor.
- Toolbar/ToolbarButton/ToolbarToggle, ContextMenu (items, separators, submenus, enabled flag), ScrollView,
  FlexLayout, Label, Slider, Button, IconButton, EditableLabel, TextBox, ComboBox.

### 0.5 Editor.App shared pieces used by pages
- ListHeader: a list title with a right-aligned "add" icon + tooltip (Animation Graph layers/parameters,
  Particles systems).
- ContainerListEditor: an add / move / remove list of slots; `ElementsAsSections` renders each element as a
  nested PropertyGrid section with remove icon (and move up/down when reorderable); supports pick dialogs,
  drag-drop of assets onto slots (`OnAssignSlot`) or onto the list (`OnAppendDropped`), and rejected-drop
  notices (`AssetPickerSlot::RejectionText`).
- ResourceRefEditor / CompactAssetSlot: an asset reference row (pick button opens AssetPickerDialog, drop target,
  clear). CompactAssetSlot is the narrow, toolbar-friendly variant (used in the animation transport rows).
- AssetPickerDialog, AssetCreateDialog, ConfirmDialog (Save / Discard / Cancel), EditorIcons (remove, etc.),
  `EditorContext::Notify(NoticeKind, text)` toasts.

---------------------------------------------------------------------------------------------------------------

## 1. Editor.Animation

Module registers three pages. Files: `Editor/Editor.Animation/{AnimationClipPage.cppm, AnimationClipPageImpl.cpp,
SkeletonPage.cppm, SkeletonPageImpl.cpp, AnimationGraphPage.cppm, AnimationGraphPageImpl.cpp,
EditorAnimationModule.cppm}`. `DrawSkeletonWireframe()` (in the graph page TU) is shared by all three.

### 1.1 Animation Clip page (`AnimationClipEditorPage`)
- Asset: `pipeline::AnimationClipAsset` (.xasset; produced by the model importer from .glb/.gltf/.fbx). No
  New-Asset creator (clips are imported).
- PURPOSE: audition an imported clip on a chosen rig, and author the few fields the source keeps: loop flag,
  root-motion extraction settings (root bone, horizontal / vertical / yaw), and animation EVENTS (time + name).
  Shows read-only stats (name, duration, track count).
- Layout: PageToolbar(Edit)+Playback on top; below a horizontal SplitView ratio 0.66:
  `left: [transport row] over [3D preview viewport] | right: property grid`.
- Transport row (left pane, top, left to right): CompactAssetSlot "Skeleton" (SkeletonAsset, 200dp),
  CompactAssetSlot "Mesh" (SkinnedMeshAsset, 200dp), time Slider (normalized 0..1 scrub, grows), time Label
  (110dp, "1.23s / 2.5s" or "(clip not cooked)").
- Preview viewport: clear colour (0.248,0.248,0.293); camera at (0,1.4,3.2) looking at (0,0.9,0). Content: a sun
  light, optional skinned mesh, debug grid (4m, 8 cells), skeleton bone wireframe (cyan lines + joint crosses),
  root-motion path polyline on the ground (orange) + a sphere at the current travel point. "Show travel" moves
  the rig along extracted root motion.
- Property grid sections, top to bottom:
  - "Clip": Name (read-only text), Duration (read-only "N s"), Tracks (read-only count), Looping (bool).
  - "Root Motion": Root bone (string), Horizontal, Vertical, Yaw (bools), Show travel (bool, page state, NOT asset
    data: it sits among asset fields with no visual distinction).
  - "Events": ContainerListEditor header (count + add icon; add places an event at the current playhead time
    named "event"), then one nested section per event "Event N" with remove icon: Time (s) float (0..duration,
    step 0.01), Name string.
- Commands: Save (writes + requests cook so bound proxies hot swap), Undo/Redo (coalesced whole-asset blob
  snapshots keyed per field), Discard, Play/Stop/Restart. Scrubbing the slider pauses playback. Play at the end
  restarts. No keyboard shortcuts specific to the page; no frame stepping; no speed control; no event markers on the
  scrub bar.
- States: asset unreadable -> page opens empty (log only); clip not cooked -> time label "(clip not cooked)" and
  nothing animates; no skeleton picked -> viewport shows only grid (no prompt text). Playing defaults to TRUE on open.
- Interactions: drag-drop of Skeleton/Mesh assets onto the compact slots; preview rig persists per clip in the
  project editor settings (ClipPreviewSettings).
- Shared: PreviewViewport, PageToolbar, CompactAssetSlot, ContainerListEditor, PropertyGrid. Bespoke: transport row,
  root-motion path overlay.
- PROBLEMS: read-only stats look like editable text fields; events are edited as numbers only (no markers on the
  timeline, no drag, no "add at playhead" feedback, order meaningless but shown as Event 1..N by insertion); the
  scrub slider is normalized with no ticks/frames; no indication that a skeleton is needed; "Show travel" mixed in
  with asset data; playback auto-starts on open.

### 1.2 Skeleton page (`SkeletonEditorPage`)
- Asset: `pipeline::SkeletonAsset` (.xasset; imported). Read-only viewer: Save is a no-op, no creator.
- PURPOSE: inspect an imported bone hierarchy and bind pose; find a bone and read its index / parent / bind TRS.
- Layout: NO page toolbar. Outer SplitView 0.78: `[inner split 0.26: left column | center preview] | right
  property grid`. i.e. `left: stats + bone tree | center: 3D viewport | right: bone info grid`.
- Left column (padding 6/4): stats Label ("Name | N bones | R roots | depth D" or "(skeleton not cooked yet)"),
  DraggableTreeView (drag disabled, 22px rows, EditableLabel rows with rename disabled, all nodes expanded).
- Center: PreviewViewport (same clear colour and camera as the clip page). Draws grid + bind-pose wireframe; the
  selected bone gets an orange cross + wire sphere.
- Right grid section "Bone" (all read-only StringEditors): Name, Index, Parent ("(root)"), Children (count), Bind
  Position, Bind Rotation (raw quaternion xyzw), Bind Scale.
- Interactions: click a tree row selects the bone (single select only). No search/filter in the tree, no
  click-to-pick bones in the viewport, no collapse-all/expand-all, no context menu.
- States: product resolves late / hot reload -> tree + info rebuild (pointer-identity watchdog); nothing selected ->
  empty grid with no hint.
- PROBLEMS: no toolbar at all (only page in the animation module without one); quaternion shown raw; read-only
  values in editable-looking fields; bone tree has no search for large rigs; viewport selection not supported;
  numbers unformatted (full float precision).

### 1.3 Animation Graph page (`AnimationGraphEditorPage`)
- Asset: `pipeline::AnimationGraphAsset` (.xasset). Header comment mentions an "Animation Graph" New-Asset creator,
  but `RegisterAnimationGraphEditor` registers only the factory (creator presumably elsewhere or missing).
- PURPOSE: author layered state machines (states = clip / 1D blend tree / 2D blend tree; transitions with
  duration, exit time, priority, conditions on parameters; Any State; default state; layer blend mode/weight) and
  a parameter table (Float/Int/Bool/Trigger with defaults), and test them live on a rig with live parameter values.
- Layout: PageToolbar(Edit)+Playback+toggles. Content: rightSplit 0.74 `[leftSplit 0.18: left list | center] |
  inspector`; center is a VERTICAL split 0.62: `top: node graph canvas / bottom: [transport row] over [3D preview]`.
  i.e. `left: Layers + Parameters lists | center top: state-machine canvas, center bottom: preview | right:
  inspector`.
- Toolbar (left to right): Save, Undo, Redo, Discard | Play, Stop, Restart | toggles "Bones" (skeleton wireframe),
  "Mesh" (skinned mesh visibility), "Travel" (walk by root motion, resets position when toggled).
- Left panel (ScrollView, rows are full-width buttons 24dp):
  - ListHeader "Layers" + add icon ("Add layer"): adds "Layer N" and selects it.
  - One row per layer; the selected layer gets the `accent` class AND a "  <" suffix appended to its text.
    Right-click: context menu [Delete] (only when more than one layer).
  - ListHeader "Parameters" + add icon ("Add parameter"): adds "ParamN" Float.
  - One row per parameter (accent when selected). Right-click: [Delete].
- Canvas (NodeGraphCanvas, StraightNodeToNode): node 0 "Any State" (teal header, not deletable); states as nodes
  (title = name, subtitle = kind "Clip"/"Blend Tree 1D"/"Blend Tree 2D", header orange for default state else blue,
  140-150 x 44-46). Transitions = straight arrowed edges. Active state during preview gets a highlight ring.
  - Canvas context menu (empty space): Add Clip State, Add Blend Tree 1D, Add Blend Tree 2D (placed at click).
  - Node context menu: Make Transition (starts a pending link; next left click on a node completes, Esc cancels);
    for real states also: Set as Default State, separator, Delete State.
  - Connection context menu: Edit Transition, separator, Delete Transition.
  - Gestures: drag nodes (positions persist on the asset, undoable "move-node"), double-click a state selects it,
    Delete key removes a selected transition (states are not deletable by key: IsDeletable=false), Ctrl+A,
    box-select, MMB pan, wheel zoom. Transitions cannot target Any State. Selecting empty space clears the inspector.
- Preview strip (center bottom): transport row = CompactAssetSlot Skeleton, CompactAssetSlot Mesh, status Label
  (current state name + "(transitioning)", or "pick a skeleton to preview" / "(skeleton not cooked yet)"); then the
  PreviewViewport (grid + optional wireframe + optional skinned mesh; travel wraps back to centre past 6 m).
- Inspector (right): title Label ("Layer", "Parameter", "State", or "Src  ->  Dst" for a transition) over a
  PropertyGrid. Per selection kind:
  - Layer: section "Layer" (remove icon when >1 layer): Name, Blend Mode (Override/Additive), Weight (0..1),
    Default State (enum by state name).
  - Parameter: section "Parameter" (remove icon): Name, Type (Float/Int/Bool/Trigger), Default (typed); section
    "Live (preview)" when a player exists: Value (typed, writes the running player only, not undoable) or
    "Fire Trigger" button.
  - State: section "State": Name (structural, relabels node), Speed, Loop; section named after kind: Clip
    (ResourceRefEditor, AnimationClipAsset) for clip states; for blend trees Parameter (1D) or Parameter X/Y (2D)
    dropdowns ("(none)" + params), then "Entries" ContainerListEditor (add icon) with nested "Entry N" sections
    (remove icon): Threshold (1D) or Position Float2 (2D), Clip (ResourceRefEditor).
  - Transition: section "Transition": Duration (s), Has Exit Time, Exit Time (0..1), Priority (-100..100),
    "Conditions" list (add icon) with nested "Condition N" sections: Parameter dropdown, Compare (== != > < >= <=),
    Threshold (float, even for Bool/Int params).
- States: asset unreadable -> empty page + "(no asset)" title; no skeleton -> status text prompt; preview rebuilds the
  runtime graph after every edit; Stop = rebuild (rewind). Playing defaults TRUE.
- Undo: whole-asset blob snapshots, coalesced by merge key; structural edits deferred via the UI mutation queue and
  reselect by identity.
- Shared: PreviewViewport, PageToolbar, NodeGraphCanvas, ListHeader, ContainerListEditor, ResourceRefEditor,
  CompactAssetSlot. Bespoke: GraphListRow (button row with RMB Delete), inspector builders, live parameter rows.
- PROBLEMS: layers/parameters are plain buttons, not a list control (no keyboard navigation, no inline rename, no
  reorder, no drag of a parameter onto a condition); selected layer marked by a text suffix "  <" (hack) as well as
  accent; deleting a state is only via context menu (Delete key silently ignored for states); conditions always use a
  float threshold even for Bool/Trigger; no validation (unreachable states, missing clips, transitions without
  conditions or exit time); context menus for nodes/connections open at approximate anchors (node header +16, or a
  fixed point 20,20 for connections) rather than at the cursor; no blend-space visualizer for 2D blend trees; no
  minimap / frame-all; preview and canvas compete for vertical space in the center column; inspector title text
  changes meaning (kind vs route).

---------------------------------------------------------------------------------------------------------------

## 2. Editor.Particles: Particle Effect page (`ParticleEffectEditorPage`)
- Files: `Editor/Editor.Particles/{ParticleEffectPage.cppm, ParticleEffectPageImpl.cpp, EditorParticlesModule.cppm}`.
- Asset: `pipeline::ParticleEffectAsset` (.xasset). "Particle Effect" New-Asset creator per header comment.
- PURPOSE: author a multi-system particle effect: per system the general/render settings, texture or mesh +
  materials, LOD, flipbook, trail; the emitter (continuous/burst); an ordered stack of initializer modules and
  behaviour modules (forces, collision, over-lifetime curves). Watch it run live with particle counts.
- Layout: PageToolbar(Edit)+Playback. rightSplit 0.72 `[leftSplit 0.22: tree column | center column] | inspector
  column`. i.e. `left: Systems tree | center: transport row over live preview | right: inspector`.
- Left column: ListHeader "Systems" + add icon ("Add system"), DraggableTreeView (22px rows, everything expanded):
  `Effect > System N (or name) > { Emitter, Initializers (count) > modules, Behaviors (count) > modules }`.
  Module rows show the reflected TYPE NAME (e.g. `PositionInitializer`), not a friendly name.
  - Inline rename: only System rows (double-click / slow-click).
  - Drag reorder: only within the same folder of the same system (initializers among initializers, etc.).
  - Context menus (RMB on row):
    - Effect: Add System.
    - System: Add Initializer > {Position, Velocity, Lifetime, Color, Size, Rotation, Mesh Orientation};
      Add Behavior > {Gravity, Drag, Wind, Turbulence, Vortex, Attractor, Radial Force, Collision, Color/Life,
      Alpha/Life, Size/Life, Rotation/Life, Speed/Life}; separator; Delete System (only when >1 system).
    - Initializers folder: Add Initializer submenu. Behaviors folder: Add Behavior submenu.
    - Initializer / Behavior: Move Up (disabled at top), Move Down (never disabled at bottom), separator, Delete.
- Center column: transport row (Label "Speed", Slider 0..3 default 1 at 90dp driving `SetTimeScale`, stats Label
  growing: "N systems | alive/cap particles | PAUSED"), then PreviewViewport (simulation ON, clear colour
  (0.272,0.272,0.313), camera (0,2,6) looking at (0,1,0), sun light). Overlay: emission-shape gizmo of the selected
  system's first Position initializer (sphere/box/cone/circle/edge/point cross, cyan).
- Right column: title Label ("Systems: N", always, regardless of selection) over PropertyGrid. Per selection:
  - Effect: "Effect" section with read-only "Systems" count only.
  - System / Initializers folder / Behaviors folder -> system inspector. Sections: "General" (remove icon when >1
    system): Name, Simulation (CPU/GPU/Auto), Sim Space (World/Local), Blend Mode (Alpha/Additive/Premultiplied/
    Multiply), Render Mode (Billboard/Stretched/Horizontal/Vertical/Mesh/Trail/Light; changing it rebuilds the
    inspector), Max Particles, Sort Particles, Soft Particles, Soft Distance, Prewarm Time; "Texture" (textured
    modes only): Texture ResourceRefEditor; "Mesh" (Mesh mode): Mesh ResourceRefEditor (Static or Skinned), Mesh
    Scale, "Materials" ContainerListEditor (slot 0 labelled "(whole mesh)"; pick opens AssetPickerDialog, drag-drop
    assign/append, move up/down, remove; rejected drops toast a warning); "LOD": Start Distance, Cull Distance, Min
    Rate; "Flipbook" (billboard family): Enabled, Columns, Rows, FPS, Over Lifetime, Start Frame; "Trail": Enabled,
    Max Points, Record Interval, Lifetime, Width Start/End, Min Vertex Dist, Use Particle Color, Trail Color.
  - Emitter: "Emitter": Mode (Continuous/Burst/Continuous + Burst; rebuilds), Spawn Rate (continuous), Duration (s),
    Looping, Burst Count / Burst Interval / Burst Cycles (0=inf) (burst).
  - Module: a section named by the reflected type name, with typed rows. Ranges are split into "X min"/"X max" rows;
    colour ranges into "start"/"end"; emission shape is a sub-form (shape enum + radius + box extents + cone angle +
    arc + from shell, ALL shown regardless of shape); Collision has Plane/Sphere/Box COUNT ints followed by fixed
    "Plane i"/"Sphere i"/"Box i" sections; over-lifetime modules embed a CurveCanvas (120dp, auto-fit values) or a
    GradientEditor (40dp) inside a property row.
- Commands: Save (write + recook), Undo/Redo (whole-effect blob, coalesced by key), Discard, Play/Stop/Restart.
  Stop clears particles and stops emission; Restart resets and plays. Speed slider is page state (not saved, no
  readout of value).
- States: unreadable asset -> empty tree/inspector; no resource manager -> mesh systems do not render; "PAUSED"
  appended to stats while paused.
- Shared: PreviewViewport, PageToolbar, DraggableTreeView, PropertyGrid, ListHeader, ContainerListEditor,
  ResourceRefEditor, AssetPickerDialog, CurveCanvas, GradientEditor. Bespoke: ParticleTreeRow, CurveFieldEditor,
  GradientFieldEditor, emission gizmo, stats line.
- PROBLEMS: raw C++ type names in tree and section titles; module enable/disable toggle absent (only delete); emission
  shape shows every parameter for every shape; collision primitives edited via count + fixed sections rather than a
  list; Move Down not disabled at the end; inspector title constant "Systems: N" carries no selection context; folder
  selection shows the parent system's inspector (surprising); speed slider has no value readout or reset; no
  per-system solo/mute or visibility in preview; gizmo is display-only (no direct manipulation); curve canvases cramped
  inside grid rows with no expand-to-large editor; no search for modules; context menu is the only way to add modules
  (no "+" in folder rows); BUG: the Color/Life GradientFieldEditor does not handle `OnStopColorRequested`, so an
  existing colour stop cannot be recoloured (new stops only inherit the sampled ramp colour).

---------------------------------------------------------------------------------------------------------------

## 3. Editor.PropertyAnimation: in-scene Property Animation panel (not a page)
- Files: `Editor/Editor.PropertyAnimation/{PropertyAnimationEditorModule.cppm, PropertyAnimationPanel.cppm,
  PropertyAnimationPanelImpl.cpp, ClipEditorView.cppm, ClipEditorViewImpl.cpp}`.
- Asset: `PropertyAnimationClipAsset` (.xasset). There is NO standalone page: the registrar only ensures the asset
  types exist (creator "New Asset -> Property Animation Clip" per comment). The SCENE page owns the panel, docked in a
  BottomDock splitter below the viewport (collapse is the dock's job). Double-opening a clip asset from the browser
  therefore does not open a page of its own (routing is through the scene page's `RequestEditClip`).
- PURPOSE: keyframe reflected component properties (f32 / Float3 / Color / Quaternion; Transform TRS built in) of one
  explicitly BOUND entity over time, scrub/play to preview on that entity in the scene (non-destructive snapshot and
  restore), save the clip asset.
- Layout (vertical FlexLayout, padding 6, spacing 4):
  1. Header row (26dp, only when a clip is loaded), text buttons: Create..., Open..., Save, + Tracks, + Track; clip
     Label (grows: "Clip: Name *   [status flash]"); entity Label (170dp: "Entity: name | (none) | (missing) |
     (unnamed)"); Bind... (opens the scene's EntityPickerDialog); Use Selected.
  2. Empty state (exclusive with body): "No animation clip selected for editing." + Create Clip..., Open Clip....
  3. Body: transport row (26dp): Play, Pause (label toggles to "Resume"), Stop, "Loop: on/off" (text-labelled toggle
     button); Timeline dopesheet (label gutter 140, one lane per track "Component.property", height = ruler + lanes,
     clamped min/max, no scroll); ClipEditorView (grows).
- ClipEditorView rows (selected track only):
  - Transport row: "Length" label + float field (clip duration, clamped to last key, one undo step) + sampled value
    readout ("sel Comp.prop @t s=... | ch=...").
  - Selected-track strip: one "Component.property.path" text field (first dot splits), kind cycle button
    (Float/Float3/Color/Quat, click cycles), interpolation cycle button (Step/Linear/Cubic applied to ALL keys; not for
    Quat), Key (capture bound entity's value at playhead), Key All, "x" (delete track, no confirm). Placeholders:
    "(no tracks - + Track adds one)", "(select a track in the dopesheet)".
  - Key inspector row: "(click a key to edit; Key captures the scene value at the playhead)" or Del, Time field, and
    per-channel fields (V | X Y Z | R G B A | P Y R degrees for quats).
  - Scalar track: one CurveCanvas whose time axis follows the dopesheet zoom/scroll (shared TimeAxis); Quat track: a
    "Keys" row of clickable chips "@t" ("[@t]" when selected).
- Commands / gestures: dopesheet scrub (ignored while playing), key drag (one undo step), lane click selects track,
  box/Ctrl selection; curve canvas gestures (section 0.4). "+ Track" opens a ContextMenu of the bound entity's
  animatable leaves not yet tracked (or "(bind an entity first)" / "(all properties already tracked)"). "+ Tracks"
  adds all at once as one undo group. Create -> AssetCreateDialog ("Create Animation Clip", placeholder "clip name");
  Open -> AssetPickerDialog (PropertyAnimationClipAsset). Dirty guard: ConfirmDialog "Unsaved Clip" with Save /
  Discard / Cancel.
- States: empty (no clip); dirty star "*" in header; status flash for a few seconds ("saved", "save FAILED (see log)",
  "save: no clip loaded", "bind: nothing selected"); editing locked while the scene simulates (playback + preview stand
  down); preview marker "preview" text drawn in 3D over the previewed entity (DrawOverlay).
- Undo: routes through the SCENE page's command stack, but the clip's dirty/save is separate from the scene's Save.
- Shared: Timeline, CurveCanvas, ContextMenu, AssetCreateDialog, AssetPickerDialog, ConfirmDialog. Bespoke: text
  button chrome, chip row, cycle buttons.
- PROBLEMS: transport is bespoke text buttons (Play / Pause / Stop / "Loop: on") instead of the shared playback
  actions; its own Save button separate from the scene page's Save (two saves, two dirty notions, shared undo stack);
  most failures go to the LOG only (key capture errors) with no in-panel feedback; cycle-buttons hide the available
  options; the track path is a free text field (easy to break); "x" deletes a track with no confirm and an
  unlabelled glyph; the dopesheet cannot scroll for many tracks; interpolation applies to all keys of a track (no
  per-key); Quat keys only as chips, no curve; keyboard: no shortcuts for key / delete key / play.

---------------------------------------------------------------------------------------------------------------

## 4. Editor.Audio

Files: `Editor/Editor.Audio/{AudioClipPage.cppm, AudioClipPageImpl.cpp, SoundCuePage.cppm, SoundCuePageImpl.cpp,
BusLayoutPage.cppm, BusLayoutPageImpl.cpp, AudioClipThumbnailGenerator.cppm}`. `RegisterAudioClipEditor` registers
the clip and cue factories plus the waveform thumbnail generator; the bus layout factory registers separately.

### 4.1 Audio Clip audition page (`AudioClipEditorPage`)
- Asset: `pipeline::AudioClipAsset` (.xasset) wrapping a source file (.wav / .ogg / .mp3) in the project Sources root.
- PURPOSE: hear an imported clip through the GAME's runtime audio engine (same buses and voice pool) and see its
  waveform and basic facts (channels, sample rate, duration, loop). Import options (gain, loop, loop points) are NOT
  edited here: the comment says "options edit via inspector" (i.e. elsewhere; Save is a no-op).
- Layout: single vertical column (spacing 8, no padding, no splitter):
  1. PageToolbar(None)+Playback: Play, Stop, Restart | Label "Vol" | Slider 0..1 step 0.05 (90dp; audition-only gain,
     not persisted) | status Label ("Playing...  1.2 s", "Paused", "No voice (engine headless?)").
  2. Info Label 13pt: "2 ch  |  48000 Hz  |  3.21 s  |  loops" or "Source file missing or undecodable.".
  3. WaveformView 160dp fixed height (custom OnDraw).
  The column does not grow: the page is a short strip at the top with empty space below.
- WaveformView OnDraw: dark fill (0.10,0.11,0.13); 256 symmetric peak bars around the midline in teal
  (64,200,190, 0.9 alpha); playhead = 2px yellow (0.95,0.85,0.4) line driven by the voice's true cursor (honours
  pitch and loop wrap). No interaction: no click-to-seek, no zoom, no selection, no loop-region display, no time ruler,
  no per-channel display.
- States: missing source -> info text only, Play disabled (CanPlay false); headless engine -> status text; playback
  ends -> playhead parks, status clears; closing the page stops the voice.
- Shared: PageToolbar, IPlaybackPage. Bespoke: WaveformView.
- PROBLEMS: page cannot edit the clip's own options (gain / loop / loop points) even though it shows them; loop
  points not visualized; no seek; volume slider has no value readout; large empty area under the strip; Save
  never dirty, so Ctrl+S (file.save, enabled only when dirty) is inert here.

### 4.2 Sound Cue page (`SoundCueEditorPage`)
- Asset: `pipeline::SoundCueAsset` (.xasset).
- PURPOSE: define a randomized sound event: up to EIGHT variant slots (clip + weight), a selection mode
  (Random no-repeat / Random / Sequential), pitch and volume jitter ranges; audition it exactly as the runtime resolves
  it (ResolveSoundCue: weights, no-repeat state, jitter).
- Layout: single vertical column (spacing 6), no ScrollView, no PropertyGrid:
  1. PageToolbar(Edit)+Playback: Save, Undo, Redo, Discard | Play, Stop, Restart | status Label (pick readout
     "slot 3  pitch 1.02  vol 0.95  |  0.4 s", or "No playable variant.").
  2. Eight fixed rows: ResourceRefEditor view "Clip" (AudioClipAsset; empty text "(empty)"; pick/drop/clear) grows +
     Label "weight" + NumericField 0..100.
  3. One row: mode Button cycling text "Mode: Random (no repeat)" / "Mode: Random" / "Mode: Sequential", then
     "pitch min", "pitch max", "vol min", "vol max" Labels + NumericFields (0..4, 2 decimals).
  4. Empty hint Label in amber (0.9,0.75,0.35): "Empty cue - assign at least one clip." while all slots are empty.
- Commands: Play audition picks a NEW variant each time (Restart re-rolls); Discard restores the last-saved snapshot
  into widgets and clears undo history (overrides the default). Save writes + `OnCookRequested(false)` (a different
  call than other pages' `RequestCook`).
- Undo: blob snapshots; weight/jitter coalesce per field; pick/clear/mode never coalesce.
- Shared: PageToolbar, ResourceRefEditor, NumericField. Bespoke: whole layout.
- PROBLEMS: fixed eight slots always shown (no add/remove, no ContainerListEditor, unlike every other list in the
  editor); weights have no normalized-probability readout; cycle button hides options (should be a dropdown); min/max
  pairs not validated (min > max allowed); hand-built rows instead of PropertyGrid (different spacing, labels, no
  sections); no per-slot audition button; no visual of which slot played beyond the status text.

### 4.3 Audio Bus Layout page (`AudioBusLayoutEditorPage`)
- Asset: `pipeline::AudioBusLayoutAsset` (.xasset). Creator lives in the editor executable's creator set.
- PURPOSE: author the mixer topology: four fixed buses (Master > Effects, Music, UI) plus custom buses from a fixed
  slot bank (kAudioCustomBusSlotCount), each with mix (volume, mute) and effect parameters (lowpass, highpass, delay,
  reverb); custom buses get a name and a parent.
- Layout: PageToolbar(Edit) (no playback) over a horizontal SplitView ratio 0.34:
  `left: ListHeader "Buses" (+ "Add bus") over bus tree | right: inspector title Label over PropertyGrid`.
- Tree: DraggableTreeView, drag disabled, rename disabled, all expanded; rows are bus names. Click selects (single).
  RMB context menu: Add Bus; Remove Bus (custom only).
- Inspector: title = bus name. Custom buses get section "Bus" (remove icon "Remove bus"): Name (structural; silently
  REJECTS empty, fixed or duplicate names with no feedback; children follow the rename), Parent (dropdown of fixed
  buses + other custom buses, excluding self and cycle makers). All buses: "Mix": Volume (0..2), Muted; "Filter":
  Lowpass Hz (0 = off), Highpass Hz (0 = off); "Delay": Delay s (0 = off), Delay Decay; "Reverb": Reverb Wet
  (0 = off), Reverb Room Size, Reverb Damping.
- States: all custom slots used -> Add logs a warning only (no toast, add icon stays enabled); unreadable asset ->
  empty page; Master selected by default.
- Undo: blob snapshots (versioned payload), structural ops via mutation queue.
- Shared: PageToolbar, ListHeader, DraggableTreeView, PropertyGrid, ContextMenu, EditorIcons.remove.
- PROBLEMS: no live metering or audition of the mix; "0 = off" encoded in labels instead of enable toggles; rename
  rejection is silent; reparent only via dropdown (no drag in tree though the adapter is reorderable); no
  solo; no visual mixer strip (faders) despite being "the mixer editor"; slot limit invisible until it fails.

---------------------------------------------------------------------------------------------------------------

## 5. Editor.Materials: Material page (`MaterialEditorPage`)
- Files: `Editor/Editor.Materials/{MaterialPage.cppm, MaterialPageImpl.cpp, EditorMaterialsModule.cppm}`.
- Asset: `pipeline::MaterialAsset` (.xasset; the authored form IS the runtime MaterialSource). Creators "PBR Material" /
  "Unlit Material" per comment (not registered in this module's `RegisterMaterialEditor`).
- PURPOSE: pick a material's shader (built-in PBR / Unlit, or a project ShaderAsset), its pipeline state (blend, depth,
  cull), set every shader property (numbers, vectors, colours, HDR colours + intensity, textures) and see it live on a
  preview shape or any project mesh.
- Layout: PageToolbar(Edit) (no playback) over a horizontal SplitView 0.62:
  `left: 3D preview viewport | right: property grid (padding 8/6)`. Preview on the LEFT (cf. Shader page: preview on
  the RIGHT).
- Preview: PreviewViewport, simulation ON (so Time()-animated shaders move), default clear colour, camera (0,0.9,2.6)
  looking at origin, re-framed to fit the mesh's bounds (FrameBounds) when the shape/mesh changes. Sun light, default
  procedural sky/IBL from the render subsystem. No background / lighting / environment choice; no turntable.
- Property grid, top to bottom:
  - "Material": Shader (ResourceRefEditor, ShaderAsset; empty text "(built-in)"), Built-in (enum PBR/Unlit; only when no
    shader asset), Blend (Opaque/Masked/AlphaBlend/Additive/Multiply/PremultipliedAlpha), Depth (Disabled/ReadWrite/
    ReadOnly/WriteOnly), Cull (None/Back/Front). (Rows are added in grid order Material, Preview, Material: the grid
    groups by category so the pipeline rows land under "Material".)
  - "Preview" (page-local, persisted per material in project editor settings, NOT asset data): Shape (Sphere/Cube/
    Plane/Cylinder/Torus/Cone), Mesh (ResourceRefEditor StaticMesh/SkinnedMesh; "(primitive)"). Picking a shape clears
    the mesh.
  - "Properties": per property in the source's table, display name prettified: Number (RangeEditor slider+field when the
    shader declares a range or for known PBR factors Metallic/Roughness/OcclusionStrength/AlphaCutoff 0..1,
    NormalScale 0..2; else FloatEditor 0..1e9), Float2/3/4 editors, IntEditor (range from shader), ColorEditor (sRGB as
    entered), ColorHdr = ColorEditor + "<Name> Intensity" FloatEditor.
  - "Textures": one ResourceRefEditor per Texture2D/TextureCube property (TextureAsset or RenderTextureAsset), pick / drop
    / clear (clear removes the slot binding).
- Commands: Save (write + recook; all scene proxies hot swap), Undo/Redo (whole-source blob, coalesced per property
  name), Discard (default: unwinds the command stack). Per-frame refreshers pull values into rows not being edited.
- States: stale source missing a forward shader property -> LOG_ERROR only ("re-create it"); unreadable -> empty;
  picked shader not yet cooked -> warning toast "That shader has not cooked yet - save it (or fix its errors), then pick
  it again" and the pick is refused; shader recooks -> rows re-laid next frame; texture hot reload watchdog rebuilds the
  preview material.
- Shared: PreviewViewport, PageToolbar, PropertyGrid (+ RangeEditor, ColorEditor), ResourceRefEditor,
  `app::PrettifyPropertyName`, Notify. Bespoke: row-type mapping, preview shape picker.
- PROBLEMS: page-local preview settings sit in the same grid as asset data; FloatEditor default range 0..1e9 forbids
  negative values for unknown floats; HDR intensity is a separate row far from its colour semantics; no texture
  thumbnails beyond the slot; no indication which properties differ from shader defaults (no reset-to-default); no
  material preview background/lighting choice; the stale-source error is log-only; no "open shader" jump from the Shader
  row beyond the slot's edit verb.

---------------------------------------------------------------------------------------------------------------

## 6. Editor.Shaders: Shader page (`ShaderEditorPage`)
- Files: `Editor/Editor.Shaders/{ShaderPage.cppm, ShaderPageImpl.cpp, ShaderEditor.cppm, HlslLexerImpl.cpp}`.
  `RegisterHlslEditorUI` registers the HLSL lexer under "hlsl" and "hlsli" in CodeLexerRegistry.
- Asset: `pipeline::ShaderAsset` (.xasset) linked to a Sources/ `.hlsl` file.
- PURPOSE: write a custom HLSL surface or low-level shader with live compile checking (base variants, 0.6 s after the
  last key), see errors on lines, and see the shader on a sphere with its properties block laid out as a material at
  defaults; edit the asset's name and entry points.
- Layout: PageToolbar(Save) (Save only) over a horizontal SplitView 0.6:
  `left: code editor (grows) / status line / error list (96dp fixed) | right: preview viewport (grows) / property grid
  (120dp fixed)`.
- Left column: CodeEditView (HLSL lexer, breakpoints disabled, diagnostics as error markers on the file's own lines;
  included-file errors only in the list); status Label (margin 6/2): "Lit surface shader: compiles (N properties). Save
  checks every variant." or "Unlit surface shader...", "Low-level shader...", or "1 error" / "N errors"; error EditText
  (read-only multi-line; formatted diagnostics).
- Right column: PreviewViewport (simulation ON for Time(); camera (0,0.9,2.6); sphere + sun) then PropertyGrid section
  "Shader": Name, Vertex Entry Point, Fragment Entry Point (StringEditors; changing an entry point re-checks).
- CodeEditView keys (shared widget): Ctrl+F find, Ctrl+H replace, F3 find next, Ctrl+G go to line, Ctrl+/ toggle comment,
  Ctrl+A/C/X/V, Ctrl+Z / Ctrl+Y (its own delta undo), Ctrl+D, Ctrl+Space completion, Tab, Esc; unhandled keys bubble
  so Ctrl+S saves.
- Commands: Save writes the .hlsl and the asset, requests a cook (compiles every variant), re-checks, then toasts
  success "Shader saved - recooking every variant" or warning "Shader saved with errors - the last good is kept"; a
  write failure toasts an error. No Undo/Redo/Discard buttons (text undo lives in the widget; asset field edits have NO
  undo at all).
- States: no linked file / unreadable / unreadable asset -> empty editor (log only, no on-page message); errors -> the
  preview keeps the last text that compiled (no visual cue that the preview is stale); the preview draws under a
  page-private shader name so scenes keep the last cooked shader until Save.
- Shared: CodeEditView, PreviewViewport, PageToolbar, PropertyGrid, Notify. Mirrors the Script page's code + status +
  fixed error list column.
- PROBLEMS: error list is an inert text box (no click to jump to line, no severity icons, no warnings); preview stale
  state invisible; property grid fixed 120dp (cramped, no scroll hint); no preview shape/mesh choice (Material page has
  one); no material-parameter tweaking in the preview (always defaults); missing-file states only logged; asset field
  edits not undoable; no Discard.

---------------------------------------------------------------------------------------------------------------

## 7. Editor.Script (+ Editor.Script.AngelScript, Editor.Script.Luau): Script page (`ScriptEditorPage`)
- Files: `Editor/Editor.Script/{ScriptPage.cppm, ScriptPageImpl.cpp, ScriptApiBrowserImpl.cpp, ScriptCompletionImpl.cpp,
  ScriptApiSurfaceImpl.cpp}`; `Editor/Editor.Script.AngelScript/{AngelScriptEditorUI.cppm, AngelScriptEditorUIImpl.cpp}`
  (lexer "angelscript" + alias "as": C-like with triple-quoted strings and char literals);
  `Editor/Editor.Script.Luau/{LuauEditorUI.cppm, LuauEditorUIImpl.cpp}` (lexer "luau" + alias "lua": Lua-like). The
  backend modules contribute ONLY syntax tables; the page is backend-neutral.
- Asset: `pipeline::ScriptClassAsset` (.xasset) linked to a Sources/ file (`.as` AngelScript, `.luau`/`.lua` Luau);
  language id on the asset picks lexer, compile check and API surface.
- PURPOSE: edit a behaviour script with syntax colouring, live compile checking (0.6 s debounce), API-aware
  completion, an API browser of exactly what the runtime binds, breakpoints shared with Game runs, and debugger
  feedback (execution line, hover values while paused).
- Layout: PageToolbar(Save) over a horizontal FlexLayout (no SplitView):
  `left (grows): code editor / status row / error list (96dp) | right: API browser (300dp fixed, hidden by default)`.
- Left column: CodeEditView (lexer by language, gutter with clickable breakpoint markers written to the shared
  EditorContext store, error markers + diagnostics, execution-line marker that scrolls into view when paused in this
  file, hover value provider while paused); status row: status Label ("Compiled OK - class Foo" / "N compile errors")
  growing + ToggleButton "API" (shows/hides the browser); error EditText (read-only, "file:line:col: message").
- API browser (`ScriptApiBrowserView`): EditText filter (placeholder "Filter API", case-insensitive substring; a type
  match keeps the whole type) over a TreeView of types (alphabetical) > members (language-formatted signatures).
  Editor-only bindings are suffixed " [editor]" (also in completion). DOUBLE-click a row inserts its text at the
  cursor. Built lazily on first open.
- Completion: Ctrl+Space or typing; type/namespace names at top level, members after `Type.`; plus the document-word
  provider.
- Commands: Save writes source, requests cook (hot reload of live behaviours), re-checks, toasts "Script saved -
  recooking" or warning "Script saved with compile errors - last good kept". CodeEditView keys as section 6.
- States: unreadable file -> empty buffer silently; unknown language -> unstyled text and empty API; no cook backend
  -> status says so ("no cook" per comment).
- Shared: CodeEditView, CodeLexerRegistry, PageToolbar(Save), TreeView, EditText, Notify, EditorContext breakpoints.
  Same column pattern as the Shader page.
- PROBLEMS: the API browser is a fixed 300dp side panel toggled by a small text "API" button inside the STATUS row (not
  on the toolbar) and not resizable (no SplitView), unlike every other side pane; error list not clickable; Shader page
  and Script page share the pattern but different status wording and toast texts; no outline / symbol list; no
  find-in-files; no indication which breakpoints are verified; insert-by-double-click is undiscoverable (no hint).

---------------------------------------------------------------------------------------------------------------

## 8. Editor.GUI: GUI document / style sheet page (`GuiAssetEditorPage`, two kinds)
- Files: `Editor/Editor.GUI/{GuiPages.cppm, GuiPagesImpl.cpp}`.
- Assets: `pipeline::GuiDocumentAsset` (Kind::Document, source `.gml`) and `pipeline::GuiStyleSheetAsset`
  (Kind::StyleSheet, source `.css`), both .xasset linked to a Sources/ file. Two factories, one page class.
- PURPOSE: edit engine-GUI markup or a CSS sheet as text with the cook's diagnostics on lines, and see a live preview
  rendered by the RUNTIME GuiSubsystem (the game's fonts, theme and sheets) in an offscreen target. A sheet is previewed
  over editor-only preview markup stored on the asset (never cooked), seeded from stock markup (Panel, Label title, body
  Label, Button, CheckBox, Slider) or copied from a chosen document.
- Layout: PageToolbar(Save) over a horizontal FlexLayout (spacing 8; NO SplitView, fixed 50/50 by flex grow):
  - Document: `left: code editor (XML lexer) | right: [status Label] over [preview viewport]`.
  - StyleSheet: `left: code editor (CSS as PLAIN TEXT, no lexer) | right: [Button "Preview Document..." + status] /
    preview-markup code editor (XML, grow 1) / preview viewport (grow 2)`.
- Preview: `ui::viewport::ViewportView` (not PreviewViewport), clear (0.313,0.332,0.366); rendered in
  OnAfterSceneRender via GuiSubsystem::RenderPreview at the viewport's pixel size. No zoom, no resolution/device presets,
  no DPI choice, no input into the preview, no element picking/highlight.
- Rebuild: 0.35 s after typing. Validation: ValidateGuiDocument / ValidateGuiStyleSheet; diagnostics (errors and
  warnings) become line markers. Status: "OK", "Error: <first message> (+N more)" / "Warning: ...", "Does not build:
  showing the last good preview.", "No GUI in the runtime: no preview.".
- Commands: Save writes the source file (and for sheets the preview markup onto the asset), rewrites the asset, calls
  `OnCookRequested(false)`; no toast. "Preview Document..." opens AssetPickerDialog (GuiDocumentAsset) and COPIES its
  markup once into the preview editor (marks the sheet dirty). No undo buttons (text undo inside CodeEditView).
- States: no linked source -> empty editor, log error only, Save refused (log only).
- Shared: CodeEditView, XmlLexer, PageToolbar(Save), AssetPickerDialog, ViewportView. Near-duplicate of the GameUI pages
  (section 9).
- PROBLEMS: no error list (only first diagnostic in a one-line status); CSS has no syntax colouring; no tag completion
  (GameUI document page has it); editing the preview markup dirties the asset (scaffolding mixed with content); no
  splitter between code and preview; preview is non-interactive and fixed to the pane size; save gives no feedback.

---------------------------------------------------------------------------------------------------------------

## 9. Editor.GameUI

Files: `Editor/Editor.GameUI/{UIDocumentPage.cppm, UIDocumentPageImpl.cpp, UIThemePage.cppm, UIThemePageImpl.cpp}`.
Shared helpers in UIThemePage.cppm: `kStockPreviewMarkup`, `markup_diagnostics::Probe/Apply` (one XML parse error on its
line), `ReadProjectSource`, `ThemePreviewResources` (resolves @icon vector images and textures for the preview).

### 9.1 UI Document page (`UIDocumentEditorPage`)
- Asset: `pipeline::UIDocumentAsset` (.xasset) linked to a Sources/ `.sml` file.
- PURPOSE: edit game-UI markup (SML, XML syntax) with tag completion and see it rendered by the RUNTIME UISubsystem
  (game context, fonts, GameTheme, VG) in an isolated preview RootView.
- Layout: PageToolbar(Save) over horizontal FlexLayout (spacing 8, no splitter):
  `left: code editor (XML lexer, completion triggered by "<", MarkupCompletionProvider) | right: status Label over
  preview ViewportView` (clear 0.313,0.332,0.366).
- Rebuild 0.35 s after typing; XML probe marks ONE error line; MarkupLoader warnings. Status: "OK", "Warnings: <first>
  (+more)", "Parse FAILED - showing the last good preview.", "No runtime UI subsystem - preview unavailable.".
- Commands: Save writes the linked file then the asset, `OnCookRequested(false)` (validating cook; canvases hot reload);
  no toast. No undo buttons.
- States: unlinked -> empty editor + log error "re-import the .sml"; Save refused (log only).
- PROBLEMS: as section 8 (status-only diagnostics, no splitter, non-interactive preview); implementation comment is stale
  (says "multi-line EditText ... no code-editor control yet" while the header and code use CodeEditView).

### 9.2 UI Theme page (`UIThemeEditorPage`)
- Asset: `pipeline::UIThemeAsset` (.xasset) linked to a Sources/ `.sss` stylesheet.
- PURPOSE: edit a game-UI theme stylesheet and preview it applied (subtree-scoped `SetLocalStyleSheet`, cascading over
  the context's default theme) to editor-only preview markup stored on the asset.
- Layout: PageToolbar(Save) over horizontal FlexLayout: `left: SSS code editor (PLAIN TEXT: no lexer) | right: [Button
  "Preview Document..." + status] / preview-markup editor (XML + tag completion, grow 1) / preview viewport (grow 2)`.
  Same arrangement as the GUI sheet page.
- Status: "OK", "Stylesheet parse FAILED - preview uses the default theme.", "Preview markup parse FAILED - showing the
  last good preview.", "No runtime UI subsystem - preview unavailable.".
- "Preview Document..." picks a UIDocumentAsset and copies its markup in.
- PROBLEMS: SSS has no syntax colouring; stylesheet parse failure falls back silently to the default theme (preview
  looks "fine" but is not the theme); preview markup dirties the asset; status wording differs from the GUI pages
  ("Parse FAILED" vs "Does not build"); two parallel UI stacks (GUI vs GameUI) with four near-identical pages that differ
  in small, arbitrary ways (completion only on GameUI, warnings vs errors, lexers).

---------------------------------------------------------------------------------------------------------------

## 10. Editor.Fonts: Font page (`FontEditorPage`)
- Files: `Editor/Editor.Fonts/{FontEditorPage.cppm, FontEditorPageImpl.cpp, FontThumbnailGenerator.cppm}`
  (registers a font thumbnail generator alongside the page).
- Asset: `pipeline::FontAsset` (.xasset) linked to a Sources/ `.ttf` / `.otf` / `.ttc`.
- PURPOSE: choose how a font is baked (raster ramp of pixel sizes, or MSDF distance field with size and field range),
  the codepoint range and atlas dimensions, and see the atlas the cook would produce (CPU bake on a background job,
  latest-wins), with glyph count and occupancy.
- Layout: PageToolbar(Edit) over a horizontal SplitView 0.6: `left: info Label over atlas ImageView (FitCenter) |
  right: property grid (padding 8/6)`.
- Info line: "Baking preview...", "<file>  |  N glyphs @ Spx  |  atlas W x H", "No preview (source file missing,
  unparseable, or the atlas is too small for the range).", "Font failed to load.".
- Atlas view: raster = R8 expanded to RGBA (white on transparent); MSDF = decoded median distance thresholded to an
  alpha ramp (white glyphs). Shown on whatever the page background is; no checkerboard, no zoom/pan, no glyph hover,
  no sample-text rendering.
- Property grid: "Font": Family (string), Bake Mode (Raster Ramp / Distance Field (MSDF); rebuilds sections);
  "Raster Ramp" (raster only): Sizes (px) as a comma/space-separated STRING (junk input silently reverts); "Distance
  Field" (MSDF only): MSDF Size (px) 8..128, Field Range (texels) 1..32; "Glyph Range": First Codepoint, Last Codepoint
  (decimal ints 0..0x10FFFF); "Atlas": Atlas Width, Atlas Height (64..8192); "Source": File (read-only), "Browse..."
  ButtonEditor -> PathPickerDialog ("Select font file", .ttf/.otf/.ttc under Sources).
- Commands: Save (write + recook), Undo/Redo (blob, coalesced per field key), Discard. Every commit rebakes.
- Shared: PageToolbar, PropertyGrid, ImageView, PathPickerDialog, EditorJobService. Same "image preview left, grid right"
  archetype as Texture / Image / Heightfield.
- PROBLEMS: sizes as free text with silent rejection; codepoints as decimal ints (no hex, no named ranges / presets like
  Latin-1); atlas preview cannot be zoomed and is white-on-page-background (low contrast in light themes); no rendered
  sample text, which is what users actually want to judge; bake progress only in the info text.

---------------------------------------------------------------------------------------------------------------

## 11. Editor.Texture: Texture page (`TextureEditorPage`)
- Files: `Editor/Editor.Texture/{TextureEditorPage.cppm, TextureEditorPageImpl.cpp, TextureThumbnailGenerator.cppm}`.
- Asset: `pipeline::TextureAsset` (.xasset) with an external Sources/ image (.png etc., decoded through the cook's stb
  path) OR embedded RGBA8 "pixels" stream (model imports).
- PURPOSE: set the GPU-texture intent of an image: usage (Color / Normal map / Data mask / HDR), colour space, block
  compression, shape (2D, array, 3D, cubemap...), sampler filters and wraps, mipmaps, anisotropy; apply coherent
  profiles; see what format it cooks to (desktop and mobile).
- Layout: PageToolbar(Edit) over a horizontal SplitView 0.55: `left: info Label over image ImageView (FitCenter) |
  right: profile button row over property grid`.
- Info line: "W x H  |  source LDR/HDR  |  cooks to RGBA8 sRGB/Linear/RGBA32F  |  N mip level(s)" (note: this "cooks to"
  is the UNCOMPRESSED derivation and can disagree with the grid's policy-resolved "Cooks to" row), or "No preview
  (source file missing, embedded, or undecodable).", "Texture failed to load.".
- Profile row: Label "Apply profile:" + Buttons UI, Sprite, 3D Surface, Normal Map, Data Mask, Equirect Sky, Cubemap Sky
  (each one undo entry, merge key "profile" so consecutive profile clicks coalesce). Wraps poorly at narrow widths (no
  overflow handling).
- Property grid: "Content": Usage (sets colour space too), Color Space (advanced), Warning (read-only lint row, visible
  only on mismatches: "Normal/Mask/HDR maps are data - sRGB will warp the values. Set Linear." / "Color as Linear is
  unusual (fine for LUT-style data)."), Compression (Default / None / Quality), Source (read-only provenance, model
  fan-out textures only), Cooks to (read-only, e.g. "BC7 sRGB  |  mobile: ASTC 4x4 sRGB"); "Sampling": Shape, Min
  Filter, Mag Filter (Nearest / Linear / Mipmap Nearest / Mipmap Linear), Wrap U / V / W (Repeat / Clamp To Edge /
  Clamp To Border / Mirrored Repeat), Generate Mipmaps, Anisotropy (RangeEditor 1..16).
- Commands: Save (write + recook, proxies hot swap), Undo/Redo (versioned blob snapshots), Discard. Refreshers re-pull
  rows after undo without rebuilding.
- Image view: no zoom, no pan, no channel isolation (R/G/B/A), no alpha checkerboard, no mip level preview, no
  sampler preview (wrap/filter not visualized), HDR shown by plain clamp (no exposure), cubemap shows only the +X face.
- Shared: PageToolbar, PropertyGrid (+ RangeEditor), ImageView. Same archetype as Image/Font/Heightfield.
- PROBLEMS: two different "cooks to" statements on one page; the lint is a read-only text row (warning styled like data);
  profiles are buttons with no indication of which profile matches current settings; preview lacks the basic image
  inspector tools (zoom, channels, alpha, mips); Wrap W shown for 2D textures; Mag Filter offers mipmap modes.

---------------------------------------------------------------------------------------------------------------

## 12. Editor.Image: Image page (`ImageEditorPage`)
- Files: `Editor/Editor.Image/{ImageEditorPage.cppm, ImageEditorPageImpl.cpp}`.
- Asset: `pipeline::ImageAsset` (.xasset) linked to a Sources/ image (the raw source Texture assets build on).
- PURPOSE: inspect the raw source image and set its one authored field, the colour-space intent; read source facts.
- Layout: PageToolbar(Edit) over a horizontal SplitView 0.6 (Texture uses 0.55): `left: info Label over ImageView
  (FitCenter) | right: property grid`.
- Info: "<file>  |  W x H  |  RGBA8  |  N KiB" or "No preview (source file missing or undecodable).", "Image failed to
  load.".
- Grid: "Import": Color Space (sRGB / Linear); "Source" (read-only StringEditors): File, Dimensions, Pixel Format,
  Data Size (the same facts as the info line, duplicated).
- Commands: Save, Undo/Redo, Discard (the Discard toolbar button with a single field).
- PROBLEMS: facts duplicated in the info line and the grid; same missing image-inspector tools as Texture; the page
  is nearly empty (one field), so a full Edit toolbar + splitter is heavy; colour space has no visual effect on the
  preview to confirm the choice.

---------------------------------------------------------------------------------------------------------------

## 13. Editor.Heightfield: Heightfield page (`HeightfieldEditorPage`)
- Files: `Editor/Editor.Heightfield/{HeightfieldEditorPage.cppm, HeightfieldEditorPageImpl.cpp,
  HeightfieldThumbnailGenerator.cppm}`.
- Asset: `pipeline::HeightfieldAsset` (.xasset), optionally linked to a 16-bit height image (LoadImage16); blank when
  unlinked. Sculpting happens in the scene viewport, not here; the 3D view belongs to the terrain page.
- PURPOSE: set the heightfield grid size (64k+1 hint), world footprint (XZ metres) and height range (min/max Y), and
  inspect the height image with its effective world height range.
- Layout: PageToolbar(Edit) over SplitView 0.6: `left: info Label over grayscale ImageView (16-bit to 8-bit by >> 8)
  | right: property grid`.
- Info: "<file>  |  source W x H  |  extent X x Z m  |  height lo .. hi m" or "Blank heightfield (flat)  |  N x N grid
  |  extent X x Z m", "Heightfield failed to load.".
- Grid: "Heightfield": Grid Size (64k+1) int 65..100000 (no validation of the 2^k+1 rule), World Size (XZ) Float2,
  Min Height, Max Height (no min<max check); "Source": File ("(blank)") and Source Size (read-only).
- Commands: Save, Undo/Redo (ApplyBlob REBUILDS the grid, unlike Texture's refreshers), Discard.
- PROBLEMS: the 8-bit display discards height precision; no contour / colour ramp / hillshade; no relink/browse for the
  source image (Font has Browse..., this does not); label "Grid Size (64k+1)" is cryptic; no validation feedback.

---------------------------------------------------------------------------------------------------------------

## 14. Editor.Generic: Generic asset form page (`GenericAssetEditorPage`)
- Files: `Editor/Editor.Generic/{AssetFormPage.cppm, AssetFormPageImpl.cpp}`.
- Asset: FALLBACK for every type without a bespoke page (factory PrimaryType = `ISerializable`; nearest-base dispatch
  lets bespoke pages win). Any .xasset.
- PURPOSE: let a user inspect and edit any asset's serialized fields even when no one has built a page for it; avoids
  the hard "No editor registered" failure.
- How: a scanning serializer runs the object's Serialize to record an ordinal field list (scalars, strings, guids,
  blobs, array counts); an edit replays Serialize with one field patched, re-scans, and rebuilds the grid only if the
  shape changed (a conditional branch flipped).
- Layout: PageToolbar(Edit) over a single column (padding 8/6): info Label ("<TypeName>  (generic editor)") over a
  PropertyGrid. No preview, no splitter.
- Grid: top-level fields in "Properties"; fields inside arrays grouped by the OUTERMOST array key as a category that is
  COLLAPSED by default (big manifests cost nothing until opened). Labels are raw serializer keys ("key[i]" for unkeyed
  elements). Rows: Bool -> BoolEditor; floats -> FloatEditor (-1e12..1e12, 4 decimals); ints -> IntEditor (+-2^53);
  String -> StringEditor; Guid -> ResourceRefEditor accepting ANY asset type PLUS a second "<key> id" text row with the
  canonical guid (TryParse; invalid input silently ignored); Blob -> read-only "(blob, N bytes)"; array counts hidden
  (arrays cannot be resized).
- Commands: Save (write + recook), Undo/Redo (blob, coalesced per label#ordinal), Discard.
- PROBLEMS: raw key labels (camelCase, "[i]"), no enums (enum fields show as ints), no ranges, no add/remove for arrays,
  duplicate guid rows, no indication of which fields matter; but it is the safety net and should remain predictable.

---------------------------------------------------------------------------------------------------------------

## 15. Editor.Render: Settings profile page (`SettingsProfilePage`)
- Files: `Editor/Editor.Render/{SettingsProfilePage.cppm, SettingsProfilePageImpl.cpp, EditorRenderModule.cppm}`.
- Assets: any `pipeline::SettingsProfileAsset` subclass (Environment Profile, Post Process Profile, ...; .xasset), by
  base-chain dispatch.
- PURPOSE: tune a shared settings profile (sky / environment / post-processing values) and see it applied either on a
  built-in look-dev scene or on one of the project's own scenes (seen from where that scene's page last left its
  camera); Save cooks so every scene referencing the profile updates.
- Layout: PageToolbar(Edit) over a horizontal SplitView 0.62: `left: 3D preview | right: property grid (padding 8/6)`.
- Preview: PreviewViewport; built-in scene = ground plane 14x14, three spheres (Matte, Metal, Gloss), a Cube, a sun WITH
  shadows; camera (0,1.7,5) -> (0,0.5,0). Or a project scene loaded through LoadEditorScene; if it fails, a warning is
  logged and the built-in scene shows. The profile's values are forced over the scene's own (the scene's profile
  reference is detached in the preview).
- Grid: "Preview" (page-local, remembered per profile): Scene (ResourceRefEditor SceneDocument; "(built-in)"); then the
  profile's reflected properties built by `SettingsRows` (SHARED with the scene page's settings section), skipping
  nested and `sceneOnly` properties, under `SettingsCategoryName(type)`.
- Commands: Save, Undo/Redo, Discard. `OnAssetExternallyModified`: when a scene's save wrote a profile edit, the page
  reloads, CLEARS its undo history and dirty flag silently.
- Shared: PreviewViewport, PageToolbar, SettingsRows (editor.scene), ResourceRefEditor, LoadEditorScene /
  LoadSceneViewPref. Bespoke: built-in look-dev scene.
- PROBLEMS: external modification silently drops local unsaved edits and undo history (no prompt); preview scene load
  failure only logged; no A/B (with / without profile) comparison; camera from another page's prefs can be confusing
  with no "reset view"; page-local Scene row mixed into the asset's grid.

---------------------------------------------------------------------------------------------------------------

## 16. Editor.Geometry: Mesh viewer page (`MeshEditorPage`)
- Files: `Editor/Editor.Geometry/{MeshPage.cppm, MeshPageImpl.cpp, EditorGeometryModule.cppm}`.
- Assets: `pipeline::StaticMeshAsset` and `pipeline::SkinnedMeshAsset` (.xasset; imported from .glb/.gltf/.fbx). Two
  factory registrations, one page. VIEWER: Save is a no-op.
- PURPOSE: look at the cooked mesh (orbit preview under a neutral or chosen material), check counts, bounds, skinning,
  submesh material indices, and LOD chain (force a LOD level to see it).
- Layout: NO page toolbar. Horizontal SplitView 0.66: `left: 3D preview | right: scrolling stats column (padding 8/6)`.
- Preview: PreviewViewport, default clear colour, camera (4,3,6) -> origin, re-framed to bounds on (re)cook; sun, default
  sky; default PBR "MeshPreview" material or the picked one.
- Right column, top to bottom: "Material" row (Label 64dp + ResourceRefEditor MaterialAsset, empty text "Default";
  page-local, persisted per mesh); LOD row (only when lodCount > 1): Buttons "Auto", "LOD 0".."LOD N" (the active one is
  DISABLED to read as pressed) driving MeshComponent.forceLod; stat Labels: "Name: ...", "Vertices: N", "Indices: N",
  "Submeshes: N", "Bounds: x x y x z", "Skinned: yes/no", per submesh "  [i] material M  |  N indices", "LOD levels: N",
  per LOD "  LOD l: T triangles  |  below C coverage".
- States: not cooked -> "Not cooked yet - the preview appears once the asset cooks."; hot reload watchdog by product uid.
- Shared: PreviewViewport, ResourceRefEditor. Bespoke: stats as plain Labels (not a PropertyGrid; differs from the
  Skeleton viewer which uses a grid of read-only rows).
- PROBLEMS: no toolbar (so no visible Save/Undo state; consistent only with Skeleton); stats are unselectable labels;
  disabled-button-as-selected is a misleading idiom (looks unavailable); no wireframe / normals / UV / bounds overlays;
  no per-submesh highlight; no skeleton overlay for skinned meshes; TODO notes a missing per-slot preview material and
  "create entity / save as prefab".

---------------------------------------------------------------------------------------------------------------

## 17. Editor.Physics: Collision shape page (`CollisionShapeEditorPage`)
- Files: `Editor/Editor.Physics/{CollisionShapePage.cppm, CollisionShapePageImpl.cpp, CollisionThumbnail.cppm,
  CollisionThumbnailImpl.cpp}` (GPU thumbnail from the cooked outline).
- Asset: `pipeline::CollisionShapeAsset` (.xasset).
- PURPOSE: choose the source mesh and cook kind (convex hull for dynamic bodies, triangle mesh for static) of a collider,
  cook it, and see the cooked outline.
- Layout: PageToolbar(Edit) over a horizontal SplitView 0.62: `left: 3D preview (wireframe outline) | right: hand-built
  column (padding 10/8, spacing 8)`.
- Right column: row "Source mesh" (Label 90dp + ResourceRefEditor Static/Skinned mesh, "(none)"); row "Cook" (Label 90dp
  + Button whose text toggles "Convex hull (dynamic)" / "Triangle mesh (static)"); row: Button "Cook now" (calls Save) +
  status Label ("No source mesh - pick one, then Cook now." / "Cooks a collider from <path>.").
- Preview: PreviewViewport with no entities or light; outline triangles drawn as green (0.30,0.95,0.55) debug lines each
  frame; camera framed when the outline's vertex count changes. Default clear colour. Empty viewport (no message) until
  cooked.
- Commands: Save / "Cook now" (identical), Undo/Redo buttons present but edits are NOT commands (always disabled),
  Discard overridden to reload from the source DB.
- PROBLEMS: Undo/Redo shown but never usable; a cycle button for a two-option enum; "Cook now" duplicates Save under a
  different name; no cook result feedback (triangle/vertex counts, errors); empty preview has no hint; hand-built
  rows instead of PropertyGrid; no overlay of the source mesh under the collider for comparison.

---------------------------------------------------------------------------------------------------------------

## 18. Editor.Input: Input map page (`InputMapEditorPage`)
- Files: `Editor/Editor.Input/{InputMapPage.cppm, InputMapPageImpl.cpp}`.
- Asset: `pipeline::InputMapAsset` (.xasset; payload = sets > actions > bindings).
- PURPOSE: define action sets (name, priority), their actions (name, kind Button / 1D axis / 2D axis, interaction On
  press / Hold / Tap / Double tap + seconds), each action's bindings (key, mouse button/axis/movement, gamepad
  button/axis/stick, four-key composite, touch button/stick) with per-binding settings, and per-action processing
  (sensitivity, gravity, curve, snap on reverse, scale with time). Rebind by pressing the input.
- Layout: PageToolbar(Edit) over a column: top bar (padding 10/6: status Label growing, dim "Layout" label, ComboBox
  "Outline" / "Two panes" 120dp), a 1px Separator, then the body in one of two layouts (switchable at runtime, not
  persisted):
  - Outline: one ScrollView; heading row "Action sets" (14pt) + count + Button "Add Set"; each set an Expander (32px
    header, bordered) whose header is an EditableLabel name (double-click to rename) and header actions: "N actions",
    Priority NumericField, add icon ("Add an action to this set"), remove icon ("Remove the set and its actions"); body:
    one rounded Card per action (action header + bindings + processing). Empty notes: "No action sets. A set groups the
    actions a game turns on together (gameplay, menus); add one to begin.", "No actions. Add one with + in the header.".
  - Two panes: left 240dp FIXED (no splitter): bar "Sets and actions" + add icon ("Add a set"), COPY icon used as "Add
    an action to the selected set", remove icon ("Remove the selected set or action"); list of ToggleButton rows (sets
    bold with "priority N"; actions indented with a kind pill "B" / "1D" / "2D"; selection = checked accent background);
    vertical Separator; right ScrollView detail: set fields (heading "Action set", Name, Priority, explanatory notes) or
    the action editor.
- Action editor: header row: name EditText (180, 14pt), spacer, kind ComboBox, interaction ComboBox, seconds NumericField
  + "s" when the interaction needs it, remove icon. "Bindings" heading + Button "Add Binding..." (ContextMenu of the
  sources valid for the kind, anchored under the button). Binding Card: source title, key cap Button(s) (four-key
  composite laid out as a keyboard cross: up over left/down/right), remove icon; kind-mismatch warning in amber
  ("A 1D axis does not take a Key binding: remove it, or change the action's kind."); settings rows with full-word
  labels and tooltips: Dead zone, Scale, Invert, Normalize, Gamepad (Any / 1-4), Screen area X/Y/Width/Height, Radius.
  "Processing" section: Sensitivity, Gravity, Curve, Snap on reverse, Scale with time.
- Rebind: click a key cap -> it reads "Press..." and the status says "Listening: press the new input (click the key cap
  again to cancel)"; capture is filtered by the action's kind; Escape is bindable so it does NOT cancel.
- Status line: listening text, "Cannot save yet: <validation error>", or "N sets · N actions · N bindings".
- Commands: Save validates first (error toast "Input map invalid: ..." and refusal); writes the asset. Unlike other pages
  it does NOT call RequestCook after saving. Undo/Redo over whole-map snapshots (field edits merge and do not rebuild);
  Discard (default stack unwind).
- Shared: PageToolbar, EditorIcons (add / remove / copy), ContextMenu, Expander, ComboBox, NumericField, CheckBox,
  EditableLabel. Bespoke: its own card visual language (rounded panels, pills, dim/warning/accent colours hard-coded
  in the page), status bar, layout switch.
- PROBLEMS: the most polished page, but its visual language (cards, pills, custom colours) exists nowhere else; two
  layouts doubles the surface to maintain and the choice is not remembered; COPY icon misused for "add action"; the
  two-pane list is not resizable; listen mode has no timeout and no keyboard cancel; no duplicate-binding conflict
  detection across sets; no search; save does not trigger a cook (inconsistent with every other page).

---------------------------------------------------------------------------------------------------------------

## 19. Script language modules (no pages of their own)
- `Editor.Script.AngelScript`: `RegisterAngelScriptEditorUI()` registers lexer ids "angelscript" and "as" (CLikeLexer:
  49 keywords, 19 types, triple-quoted strings, char literals; block comments do not nest; no preprocessor).
- `Editor.Script.Luau`: `RegisterLuauEditorUI()` registers "luau" and "lua" (LuaLikeLexer with Luau keywords and types
  any/boolean/buffer/never/number/string/thread/unknown).
- `Editor.Shaders` `RegisterHlslEditorUI()` registers "hlsl"/"hlsli".
- No lexer exists for CSS (GUI sheets) or SSS (GameUI themes): those editors are plain text.

---------------------------------------------------------------------------------------------------------------

## 20. Page archetypes (recurring shapes)

| Archetype | Pages | Shape today |
|---|---|---|
| A. 3D preview + inspector | Material, Settings Profile, Mesh viewer, Collision Shape, Animation Clip | horizontal SplitView, preview LEFT (0.62 to 0.66), grid or hand-built column right |
| B. Outline + preview + inspector (three pane) | Particle Effect, Skeleton, Animation Graph (adds a node canvas over the preview) | tree/list left (0.18 to 0.26), preview centre, inspector right (0.72 to 0.78) |
| C. 2D image preview + inspector | Texture, Image, Font, Heightfield | info label over FitCenter ImageView left (0.55 to 0.6), PropertyGrid right |
| D. Code editor (+ live preview) | Shader, Script, GUI Document, GUI Style Sheet, UI Document, UI Theme | code LEFT; preview / status / error list placed differently per page; Save-only toolbar |
| E. Form / list editor (no preview) | Generic form, Sound Cue, Audio Bus Layout, Input Map | single column or list + detail |
| F. Audition strip | Audio Clip | toolbar + info + waveform strip, no split |
| G. Embedded panel | Property Animation (in the scene page's bottom dock) | header + transport + dopesheet + selected-track editor |

Counts: 22 page classes producing 23 page variants (the GUI page serves two asset types; the mesh page is registered
for two types, counted once) for 25 asset types (incl. the Generic fallback and the SettingsProfile family), plus 1
in-scene panel (Property Animation + ClipEditorView) and 3 lexer-only registrars. Custom OnDraw widgets in scope: only
`WaveformView` (Audio); every other visual uses toolkit widgets or debug-draw lines in a PreviewViewport.

---------------------------------------------------------------------------------------------------------------

## 21. Cross-page comparison matrix

| Page | Toolbar | Playback | Layout (split ratios) | Preview side | Undo | Save -> cook | Save feedback | Discard |
|---|---|---|---|---|---|---|---|---|
| Animation Clip | Edit | yes (+ own scrub slider) | preview col / grid 0.66 | left | blob, merged | RequestCook | log only | default unwind |
| Skeleton | none | no | tree 0.26 / preview / grid 0.78 | centre | n/a (viewer) | no-op | none | n/a |
| Animation Graph | Edit + Bones/Mesh/Travel toggles | yes | list 0.18 / (canvas over preview, vertical 0.62) / inspector 0.74 | centre bottom | blob, merged | RequestCook | log only | default unwind |
| Particle Effect | Edit | yes (+ speed slider in content) | tree 0.22 / preview / inspector 0.72 | centre | blob, merged | RequestCook | log only | default unwind |
| Property Animation panel | bespoke text buttons | bespoke Play/Pause/Stop/Loop | vertical stack | scene viewport | scene page stack | own Save button | header flash | Save/Discard/Cancel on load |
| Audio Clip | None | yes (+ Vol slider) | single column | n/a | none | none | n/a | n/a |
| Sound Cue | Edit | yes | single column, hand-built rows | n/a | blob, merged | OnCookRequested | none | override: restore saved, clear history |
| Audio Bus Layout | Edit | no | tree / grid 0.34 | n/a | blob, merged | RequestCook | log only | default unwind |
| Material | Edit | no (sim on) | preview / grid 0.62 | left | blob, merged | RequestCook | log only | default unwind |
| Shader | Save | no | code col / (preview + grid 120dp) 0.6 | right | text widget only; asset fields none | RequestCook | toast | none |
| Script | Save | no | code col (flex) + API 300dp fixed | n/a | text widget only | RequestCook | toast | none |
| GUI Document / Sheet | Save | no | flex 1:1 code / right col | right | text widget only | OnCookRequested | none | none |
| UI Document / Theme | Save | no | flex 1:1 code / right col | right | text widget only | OnCookRequested | none | none |
| Font | Edit | no | image / grid 0.6 | left | blob, merged | RequestCook | log only | default unwind |
| Texture | Edit | no | image / (profiles + grid) 0.55 | left | blob, merged | RequestCook | log only | default unwind |
| Image | Edit | no | image / grid 0.6 | left | blob, merged | RequestCook | log only | default unwind |
| Heightfield | Edit | no | image / grid 0.6 | left | blob, merged (grid rebuild) | RequestCook | log only | default unwind |
| Generic form | Edit | no | single column | n/a | blob, merged | RequestCook | log only | default unwind |
| Settings Profile | Edit | no | preview / grid 0.62 | left | blob, merged | RequestCook | log only | default unwind; external change silently clears |
| Mesh viewer | none | no | preview / stats 0.66 | left | n/a | no-op | none | n/a |
| Collision Shape | Edit (Undo/Redo dead) | no | preview / column 0.62 | left | NONE (direct mutation) | RequestCook (+ "Cook now") | log only | override: reload from DB |
| Input Map | Edit | no | outline, or list 240dp fixed / detail | n/a | whole-map snapshots | NO cook | error toast on invalid | default unwind |

Other per-page constants worth unifying:
- Preview clear colours: animation (0.248,0.248,0.293); particles (0.272,0.272,0.313); material / shader / mesh /
  profile / collision use the PreviewViewport default; GUI and GameUI ViewportViews (0.313,0.332,0.366); waveform
  (0.10,0.11,0.13).
- Preview camera start poses: animation (0,1.4,3.2)->(0,0.9,0); particles (0,2,6)->(0,1,0); material/shader
  (0,0.9,2.6)->origin; mesh (4,3,6)->origin then framed; profile built-in (0,1.7,5)->(0,0.5,0); collision framed on cook.
- Live-check debounce: Shader and Script 0.6 s; GUI and GameUI 0.35 s.
- Playback state on open: Animation Clip and Graph auto-play; Particles plays; audio pages idle.
- Empty asset-slot texts: "(none)", "(empty)", "(built-in)", "(primitive)", "Default", "(none - pick a mesh)".
- Preview choices remembered per asset (project editor settings): Animation Clip and Graph (skeleton + mesh), Material
  (shape / mesh), Mesh (material), Settings Profile (scene). Not remembered: particle speed, audio volume, input layout,
  splitter positions (all pages), camera pose (all asset previews).

---------------------------------------------------------------------------------------------------------------

## 22. Inconsistencies (ranked by impact on a consistent redesign)

1. Toolbar presence and contents vary: Edit set on most pages; Save-only on all text pages; None + playback on Audio
   Clip; NO toolbar at all on Skeleton and Mesh; Collision Shape shows Undo/Redo that can never enable; the Property
   Animation panel has its own text buttons and its own Save separate from the scene page's.
2. Preview placement: left on Material / Profile / Mesh / Collision / Clip / Texture / Image / Font / Heightfield,
   right on Shader and all GUI/GameUI pages, centre on Particles / Skeleton / Graph, centre-bottom under a canvas on the
   Graph; split ratios range 0.55 to 0.66 with no shared default and nothing persisted.
3. Playback controls: shared Play/Stop/Restart on six pages, but Particles adds a speed slider in the content row, Audio
   Clip adds a volume slider in the toolbar, Animation Clip adds a normalized scrub slider in its transport row, the
   Graph adds Bones/Mesh/Travel raw toggles (not registry actions), and Property Animation uses its own Play / Pause /
   Stop / "Loop: on" buttons with a real dopesheet timeline. Auto-play on open differs per page.
4. Save semantics: most pages `RequestCook(false)`; Sound Cue, GUI and GameUI call `OnCookRequested(false)`; Input Map
   does not cook at all; Skeleton / Mesh / Audio Clip Save is a no-op; Collision adds "Cook now" which is just Save.
   Feedback: toast only on Shader and Script (and Input's invalid-map error); everything else logs.
5. Undo model and Discard: blob snapshots with merge keys on most pages; text pages rely on CodeEditView's own undo
   (asset fields on the Shader page are not undoable at all); Collision mutates directly (no undo, Discard reloads);
   Sound Cue Discard restores the saved snapshot AND clears history; Settings Profile silently clears unsaved edits on
   an external change; Property Animation's undo lives on the scene's stack while its save is separate.
6. Status and diagnostics surfaces: Script/Shader = status line + fixed 96dp read-only error box below the code; GUI /
   GameUI = one-line status above the preview showing only the first diagnostic; Input = status in a top bar; audio and
   particles = status inside the toolbar or transport row; Property Animation = transient header flash; most load
   failures ("failed to read - page opens empty", missing source files) are LOG-ONLY with no on-page empty state. No
   error list is clickable.
7. Lists are built five different ways: ContainerListEditor sections (clip events, blend entries, conditions, particle
   materials), a fixed eight-row slot bank (Sound Cue), a count field plus fixed sections (particle collision planes /
   spheres / boxes), bespoke rounded cards (Input Map), and plain button rows with RMB Delete (Graph layers and
   parameters). Add affordances: ListHeader + icon, text buttons ("Add Set", "+ Track", "Add Binding..."), context-menu
   only (particle modules), "add at playhead" (clip events).
8. Read-only information is shown three ways: disabled-looking-but-not StringEditors with empty setters in grids (Clip
   stats, Skeleton bone info, Image / Heightfield source facts, Texture lint and "Cooks to", Generic blobs), plain
   Labels (Mesh stats), and an info Label line above the preview (Texture / Image / Font / Heightfield / Audio), often
   duplicating the same facts (Image shows them twice; Texture shows two different "cooks to" answers).
9. Selection and choice idioms differ: tree selection (Particles, Bus, Skeleton), accent class plus a literal "  <"
   suffix (Graph layers), checked ToggleButtons (Input two-pane), "[@t]" text chips (Property Animation quat keys), a
   DISABLED button to mean "selected" (Mesh LOD row); enums appear as dropdowns in grids but as text CYCLE buttons on
   Sound Cue (mode), Collision (cook kind) and Property Animation (kind, interpolation, loop).
10. Page-local preview settings are mixed into the asset's property grid with no visual distinction (Material "Preview"
   Shape/Mesh, Settings Profile "Preview" Scene, Animation Clip "Show travel"), while other pages keep them in toolbars
   or transport rows (Graph toggles, clip/graph rig slots, Mesh material row, particle speed). Scaffolding data that
   IS saved on the asset (GUI sheet and UI theme preview markup) dirties the asset like real content.

Further inconsistencies:
- Inline rename exists only for particle System rows and Input set headers; everywhere else names are grid fields
  (sometimes both: particle system name is in the tree AND the General section).
- Code editors: HLSL, AngelScript, Luau and XML have lexers; CSS and SSS are plain text; tag completion only on GameUI
  markup; API completion only on scripts; breakpoints only on scripts.
- Hard-coded colours outside the theme: Input Map (dim / warning / accent), Sound Cue amber hint, waveform colours,
  node header colours in the Graph, debug-draw colours per page.
- Icons: EditorIcons.remove in section headers (Graph, Particles, Bus, Input), `copy` icon reused as "add action" in
  Input's two-pane bar, text glyph "x" for delete track and "Del" for delete key in Property Animation; toolbar buttons
  are text-only everywhere.
- Context menus open at the cursor (Particles, Bus, Graph list rows, Graph canvas) but node / connection menus in the
  Graph open at computed anchors (node header + 16px, or a fixed point for connections).
- Fixed-size side panes that cannot be resized: Script API browser 300dp, Input list 240dp, Shader grid 120dp, error
  boxes 96dp, Audio waveform 160dp; elsewhere SplitViews.
- Stale doc comments: AnimationGraph and ParticleEffect headers promise New-Asset creators their registrars do not
  register; UIDocumentPageImpl says "multi-line EditText" while it uses CodeEditView.

---------------------------------------------------------------------------------------------------------------

## 23. Cross-cutting problems a redesign should fix
- No consistent empty / loading / error states: a page that fails to read opens blank with a log line; long operations
  (font bake, shader check, cook after save) show progress only as text, if at all.
- No keyboard paths beyond the global Ctrl+S / Ctrl+Z / Ctrl+Y and the code editor: no page-level shortcuts for play /
  pause / stop, frame selection in previews, add/remove list item, rename (F2), delete in trees, focus search.
- No search or filter anywhere except the Script API browser (none in bone trees, particle module trees, bus trees,
  input actions, generic forms with hundreds of fields).
- Preview viewports share camera gestures but give no on-screen hint, no reset / frame button, no background or
  lighting choice, no stats overlay convention (only Particles shows counts).
- 2D image previews (Texture, Image, Font, Heightfield) lack zoom, pan, channel isolation, alpha checkerboard, mip and
  exposure controls, and pixel readout; the Font page shows the atlas instead of sample text.
- Bespoke hand-built rows (Sound Cue, Collision, Property Animation, Input Map, Mesh stats) diverge in spacing, label
  widths (64 / 90 / 96 / 110dp), font sizes (11 to 14pt) and control types from the PropertyGrid used elsewhere.
- Splitter positions, chosen layouts (Input), preview speeds and volumes are never remembered.
- Real bug found: GradientEditor's double-click "edit stop colour" event has no handler anywhere, so particle
  Color/Life stops cannot be recoloured.
