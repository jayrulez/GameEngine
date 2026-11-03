# UI Catalog: Scene, Play, Preview, ViewportTools, Camera, Navigation, Spline, Terrain, Vegetation

Scope: `Code/Editor/Editor.Scene`, `Editor.Play`, `Editor.Preview`, `Editor.ViewportTools`, `Editor.Camera`,
`Editor.Navigation`, `Editor.Spline`, `Editor.Terrain`, `Editor.Vegetation`. Paths below are relative to
`Code/Editor/`. Shared widgets used here but defined elsewhere (Editor.App:
`PageToolbar`, `ToolPanel`, `ToolPanelWidgets`, `ContainerListEditor`, `ResourceRefEditor`, `AssetPickerDialog`,
`EditorIcons`; editor.propertyanimation: `PropertyAnimationPanel`) are named, not catalogued in depth.

Each entry: Kind, Files, Where/how opened, Purpose, Regions and controls (top to bottom), Commands, States,
Interactions, Drawing/icons, Problems.

Contents
1. Scene Editor Page (page + layout)
2. Scene viewport toolbar
3. Overlays menu / Post menu / Debug view menu / Tool category menus (popups)
4. Scene viewport (3D) and its input
5. Viewport overlays drawn through DebugDraw (grid, axes, markers, selection box, gizmo, marquee, status text, FPS, zoom readout, view gizmo, component gizmos)
6. Camera preview overlay
7. Tool panel targets (Floating tool panel, Viewport overlay HUD, Bottom dock tabs)
8. Bottom dock (Animation tab host)
9. Hierarchy panel (+ its context menus)
10. Inspector (Entity tab, Scene tab, per-component custom sections)
11. Component Picker dialog (Add Component)
12. Entity Picker dialog
13. Confirm dialogs (Apply to Prefab, Revert Instance, Overwrite Component) and Asset picker (Spawn Prefab)
14. Notices/toasts raised by these pages
15. Viewport tools framework (palette, activation, refusals)
16. Select/Transform tool (default tool) and gizmo
17. Terrain tools: Sculpt, Paint Splat, Cut Holes (tools + floating panels)
18. Vegetation tools: Paint Vegetation, Paint Props (tools + floating panels)
19. Spline tool (no panel)
20. Navigation bake (inspector button, preference)
21. Terrain Editor Page (asset page)
22. Game page (PIE tab), its toolbar, resolution dropdown, phone toggle, debugger panel, multiple instances
23. Game audio and Navigation preferences contributions
24. Preview viewport substrate (Editor.Preview)
25. Thumbnail stage + scene thumbnail generators + project thumbnail capture
26. Editor camera (controls; no UI of its own)
27. Scene actions registry (menu "Scene/...", shortcuts)
28. Cross-cutting inconsistencies
29. Counts by kind

---

## 1. Scene Editor Page

- Kind: page (document tab). One per open SceneDocument or PrefabDocument (same class serves both).
- Files: `Editor.Scene/ScenePage.cppm` (layout in ctor, lines ~92-412), `Editor.Scene/ScenePageImpl.cpp`.
- Opened by: opening a Scene or Prefab asset (factories `SceneEditorPageFactory`, `PrefabEditorPageFactory`), File > New Scene (asset creator). Title = asset name; the shell adds the dirty marker.
- Purpose: author one scene (or one prefab template): see it rendered by the real renderer, select and
  transform entities, edit components and scene-level settings, run an in-place Simulate, and use
  in-scene modal tools (terrain, vegetation, spline). Data: the live Scene, its entities, components,
  scene system settings blocks, prefab instance state, editor view prefs.
- Layout (fixed, by code):
  - `[ Hierarchy | ( ViewportColumn | Inspector ) ]` using two nested `SplitView`s: hierarchy ratio 0.2, inspector split 0.72.
  - ViewportColumn = vertical `SplitView` (ratio 0.72): top = viewport pane, bottom = `BottomDock` (collapsed, hidden when nothing open).
  - Viewport pane = vertical Flex: toolbar strip (fixed 30dp) over a `FrameLayout` holding:
    the 3D `ViewportView` (fill, clear color 0.349/0.366/0.396), camera preview container (bottom-right, 12px margin),
    tool overlay HUD panel (top-right, 260dp wide, 8px padding, `panel` class, Gone when idle),
    tool float layer (`AbsoluteLayout`, not hit-test visible) with a `FloatingPanel` at left 16 / top 16.
- States:
  - No scene / load failure: page still builds; a failed load only logs (`LOG_ERROR "scene failed to load"`); NotFound = "new scene" log. No in-UI empty or error state. An empty scene shows only the debug grid and origin axes.
  - Dirty: via command stack; shell tab marker. External change while dirty: warning notice "'X' changed on disk but has unsaved edits here - not refreshed."
  - Simulating / paused: see toolbar; edits locked by `Commands().SetLocked(true)`; active modal tool forced back to Select.
  - Prefab document: same UI, Save validates "exactly one root" and "no self instance" via warning notices. No visual indicator anywhere that this page is a prefab and not a scene.
  - Persistence: per-scene view prefs (overlay toggles, grid plane, camera, ortho, selection, split ratios) saved on toggle and on close (`SaveViewPrefs`), restored on open.
  - Project default scene: auto-captures the launcher thumbnail after 1.5 s settled if none exists, and on every save.
- Interactions: see sections 4, 9, 10. No drag-and-drop of assets into the viewport or the hierarchy exists (no drop handlers in Editor.Scene).
- Problems:
  - No Save / Undo / Redo on the page itself (other asset pages use `app::PageToolbar` with these; the scene page builds a bare `ui::toolkit::Toolbar`).
  - Load failure is invisible in the page (log only); user sees an empty scene.
  - Prefab mode is indistinguishable from scene mode.
  - Layout is hard coded; hierarchy and inspector cannot be moved/hidden; no way to collapse panels for a large viewport other than dragging splitters.
  - No asset drag-and-drop to place meshes/prefabs: spawning a prefab needs a menu and a modal picker.

## 2. Scene viewport toolbar

- Kind: toolbar. Files: `ScenePageImpl.cpp` `BuildViewportToolbar` (~1520-1718), `ScenePage_OverlaysInit` (~1812), `SyncToolbar` (~1951), `RefreshSimToolbar` (~1781).
- Where: strip above the scene viewport, 30dp high.
- Purpose: show and change transform mode/space/snap, viewport display options, pick a modal tool, toggle the animation panel, run/pause/stop Simulate.
- Controls left to right:
  1. Translate toggle (icon `EditorIcons.translate`, no text) -> action `scene.gizmo.translate` (W).
  2. Rotate toggle (icon `rotate`) -> `scene.gizmo.rotate` (E).
  3. Scale toggle (icon `scale`) -> `scene.gizmo.scale` (R).
  4. separator
  5. Space toggle: text "World"/"Local" + icon `worldSpace`/`localSpace` (checked = world) -> `scene.gizmo.worldSpace`. Keyboard X is read by the select tool directly (no action shortcut).
  6. "Vertex Snap" text toggle (tooltip "Move the selection's pivot onto the vertex under the cursor (hold V for one drag)") -> `scene.gizmo.vertexSnap`.
  7. separator
  8. "Overlays" menu button with `grid` icon -> Overlays popup (section 3).
  9. "Post" menu button (text only) -> Post flags popup.
  10. "Debug" menu button (text only) -> Debug view popup.
  11. separator (only when more than the default tool exists)
  12. Tool palette: one menu button per category with 2+ tools ("Terrain", "Vegetation"; label becomes "Terrain: Sculpt Terrain" and checked when one is active), one toggle per lone tool ("Spline").
  13. separator
  14. "Animation" toggle -> `scene.view.animationPanel`.
  15. flexible spacer
  16. "Play" button -> `scene.simulate.start`; "Pause" toggle -> `scene.simulate.pause`; "Stop" button -> `scene.simulate.stop`.
  17. State label (13pt): empty when editing, " SIMULATING " orange (0.95,0.55,0.35) or " PAUSED " amber (0.95,0.85,0.4).
- States: Play enabled only when not simulating; Pause/Stop only while simulating. Gizmo toggles disabled while the camera owns input (Alt/RMB/Tab). Synced every frame from the action registry.
- Problems:
  - Mixed presentation: icon-only (TRS), icon+text (space, overlays), text-only (Vertex Snap, Post, Debug, tools, Animation, Play/Pause/Stop). No icons for Play/Pause/Stop.
  - "Play" here means Simulate (in-place snapshot); "Game > Play" opens the Game tab: same word, two very different operations.
  - Status label uses leading/trailing spaces for padding.
  - No camera speed control, no snap increment controls (Ctrl snap uses hard-coded 1.0 / 15 deg / 0.25 with no UI), no grid size control, no view-mode (lit/unlit/wireframe) control: Debug view is the only view-mode surface.
  - No tooltips on most buttons (only Vertex Snap has one); icon-only TRS toggles have no tooltip in this code.
  - Tool dropdown label grows ("Terrain: Sculpt Terrain") and shifts the rest of the bar.
  - No keyboard path to the tool palette (no shortcuts for terrain/vegetation/spline tools).

## 3. Toolbar popups (context-menu style)

All are `foundation::ui::ContextMenu` anchored under their button; checkable state faked with text prefixes "[x] " / "[ ] ".

### 3a. Overlays menu
- Files: `ScenePageImpl.cpp` `ShowOverlaysMenu` (~1335).
- Purpose: toggle editor-only debug draws in this viewport. Persisted per scene.
- Items: Grid | Grid on XZ (ground) | Grid on XY | Grid on YZ | Grid as lines (to compare) | Entity markers | --- | LOD overlay | Colliders | --- | FPS.
- Problems: radio group (plane) and toggles mixed with the same "[x]" marker; "Grid as lines (to compare)" is a developer comparison option exposed to users; menu closes per click so toggling several needs reopening. Entity markers is duplicated as action `scene.view.markers` ("Entity Markers") in the Scene menu with a different label case.

### 3b. Post menu
- Files: `ShowPostFlagsMenu` (~1388).
- Purpose: strip post effects / culling for this viewport only (ephemeral, not saved).
- Items: No Post (bloom/AO/SSR/AA off) | --- | No Bloom | No AO | No SSR | No SSGI | No AA (crisp) | No frustum culling (draw everything) | --- | MSAA <levels from kMsaaLevels> (radio).
- Problems: negative phrasing ("No X") means checked = effect off, opposite of the Overlays menu; not persisted while Overlays is; culling is not a post effect but lives here.

### 3c. Debug view menu
- Files: `ShowDebugViewMenu` (~1438).
- Purpose: visualize a render term or any render-graph texture in the viewport.
- Items: Final (debug view off) | --- | Albedo (base color) | Normals (world, mapped) | Roughness | Metallic | Shadow cascades | Light-cluster heatmap | Overbright / invalid | --- | one item per last-frame graph texture "name  WxH[ depth]" (multisampled ones skipped).
- Problems: no indicator on the toolbar or viewport that a debug view is active (easy to forget); the resource list can be very long with no search; depth linearization planes hard-coded 0.1/1000.

### 3d. Tool category menu
- Files: `ShowToolMenu` (~1307). Items: "[x]/[ ] <tool DisplayName>" per tool in the category; picking the active one returns to Select.
- Problems: unavailable tools are listed as enabled and only refuse after click (warning notice).

## 4. Scene viewport (3D)

- Kind: page region (`ui::viewport::ViewportView`). Files: `ScenePageImpl.cpp` OnUpdate / OnRenderWindow / UpdateViewportTools.
- Purpose: the rendered scene, the main surface for selection and manipulation.
- Mouse/keys (combined from EditorCamera, select tool, view gizmo, actions):
  - RMB hold: free look; while held WASD move, Q/E down/up, Shift fast (fixed 8 / 30 m/s).
  - Alt+LMB: turntable orbit about focus. MMB: pan. Wheel: dolly (12% of pivot distance per notch); triggers zoom readout. Shift+wheel is given to a modal brush instead.
  - Tab: toggle captured fly mode (relative mouse, hidden cursor); Esc releases.
  - LMB click: pick (GPU pick, CPU origin pick fallback); Ctrl+click toggles; click on nothing clears.
  - LMB drag on empty space past 4 px: marquee rect select (Ctrl adds).
  - W/E/R: gizmo mode; X: world/local; Ctrl during drag: snap; V held: vertex snap for one drag.
  - F: Frame Selection (eased). O or Keypad 5: Orthographic toggle.
  - Click on view gizmo knob: snap to axis (auto-ortho per user setting `AutoOrthographicViews`); click on its centre: toggle ortho.
  - Ctrl+D duplicate, Delete delete (actions, act on primary only).
- States: input only while hovered/focused/captured; keyboard dropped when editor UI focus is elsewhere. Simulate: gizmo read-only, selection still works. Hidden tab: rendering skipped.
- Problems:
  - No right-click context menu in the viewport (RMB is camera only), so create/duplicate/delete need the hierarchy, menu or shortcuts.
  - No drop target for assets.
  - Camera speed is fixed with no UI or wheel-while-flying adjustment.
  - Multi-selection exists (marquee, Ctrl+click) but gizmo, Duplicate, Delete, Copy, Frame (frame uses all) mostly act on the primary only; hierarchy shows only the primary as selected.
  - Keyboard ownership is invisible: W/E/R mean gizmo or fly depending on RMB, with no on-screen cue other than the toggles going disabled.

## 5. Viewport overlays (DebugDraw)

All drawn into `m_render->DebugView(m_viewport)` (keyed to the main viewport, so never into the camera preview). Suppressed on a clean project-thumbnail capture frame.

| Overlay | Where | Toggle | Drawing |
|---|---|---|---|
| Ground grid | plane XZ/XY/YZ centred under camera, spacing by height (`SceneGrid.cppm`) | Overlays > Grid, plane, "as lines" | `DrawGridPlane` shader grid with fade; or 201x201 debug lines, every tenth brighter |
| Origin axes | world origin, 1 m | with Grid | 3 debug lines red/green/blue |
| Entity markers | every entity origin | Overlays > Entity markers / action Entity Markers | 3-line cross 0.25 m, grey; selected = yellow (1,0.85,0.25) |
| Selection outline | selected entities | always | wire AABB of `EntityWorldBounds` (or 0.35 m cube fallback), yellow. Not a true outline, an axis-aligned box |
| Transform gizmo | primary selection | Select tool active, selection exists | ribbons, plane quads, rings, centre handle, angle guide with integer degree readout (degree sign U+00B0); vertex snap target |
| Marquee | screen rect | during drag | fill (0.35,0.6,1,0.18) + 1 px edge |
| Tool status text | top-left (12,12) | active tool StatusText | debug text; e.g. "Move [World]  (W/E/R mode, X space, Ctrl snap, V vertex snap)" |
| FPS | top-right (12,12) right-aligned | Overlays > FPS | half-second sampled text |
| Zoom readout | bottom-left, 12 margin | 1.0 s after a wheel zoom, 0.5 s fade | translucent black box, "N m to focus", "grid cell N", scale bar with ticks (`ZoomReadout.cppm`) |
| View gizmo | top-right, centre at (w-12-34-9, 28+34+9) | always | six knobs 18 px (labels X/Y/Z/-X/-Y/-Z), arms 2 px, centre square 10 px, "Persp"/"Ortho" label below; hovered knob brightened (`ViewGizmo.cppm`) |
| Component gizmos | per entity (`ComponentGizmos.cppm`) | selected only unless renderer opts in; LOD overlay + Colliders via Overlays | Light, ReflectionProbe, Decal, Camera, NavMeshZone (selected); LOD overlay, PhysicsCollider, ChildCollider, CharacterCollider, Joint (always when enabled); Spline; TwoBoneIK, AimIK, FootIK |
| Brush cursors | terrain/vegetation hover | modal tool active | two circles (radius, half radius) on the surface normal, tool-specific colours |
| Spline handles | spline entity | Spline tool | curve lines, point spheres, handle lines, insert/place previews |
| Property animation marker | live preview entity | animation panel | `PropertyAnimationPanel::DrawOverlay` |

- Problems:
  - All viewport text uses the debug bitmap font (fixed glyph size, no DPI scaling, no theme), unlike the UI.
  - Layout collisions: tool status text at (12,12) top-left sits under the Floating tool panel placed at (16,16); FPS and view gizmo share top-right with the ViewportOverlay HUD target (top-right, 12 margin, 260dp) which would cover both.
  - Selection feedback is a box, not an outline; overlapping boxes are hard to read in dense scenes.
  - Status text strings are each tool's own free-form format (see section 28).
  - No grid size/units readout except transiently during zoom.

## 6. Camera preview overlay

- Kind: viewport overlay (UI views, not DebugDraw). Files: `ScenePageImpl.cpp` BuildCameraPreview/UpdateCameraPreview/RenderCameraPreview/ToggleCameraPin (~2109-2254), `CameraPreview.cppm` (pure logic).
- Where: bottom-right of viewport, 12 px margin. Opened automatically when the primary selection has a CameraComponent, or a camera is pinned.
- Purpose: see what a scene camera sees while editing elsewhere.
- Controls: "Pin"/"Unpin" `ui::Button` (full width, 24dp) above a display-only 320x180 `ViewportView` (black clear).
- States: hidden when no camera selected/pinned; pin auto-clears when the pinned entity stops being a camera.
- Problems: no title saying which camera is shown; fixed 320x180 (16:9) regardless of the camera's aspect (comment claims aspect-driven, value fixed); cannot resize, move or close; Pin is a plain button styled differently from toolbar toggles; preview does not render post override or debug view (by design) but this is not indicated; editor overlays never show there (good) but neither do gizmos for orientation.

## 7. Tool panel targets

The `ViewportToolPanelHost` (Editor.App `ToolPanel.cppm`) mounts the active tool's registered panel at its provider's `Placement()`; `SceneEditorPage::MountToolPanel` routes (~918).

### 7a. Floating tool panel (used by every current tool panel)
- Kind: floating tool panel (`ui::toolkit::FloatingPanel`). Built in `BuildToolFloat` with title "Brush", then retitled to the active tool's DisplayName on mount, un-collapsed on each mount.
- Where: over the viewport at left 16 / top 16 in an AbsoluteLayout layer; draggable, resizable, collapsible, closable. Close = return to the Select tool.
- Problems: position is not persisted; always reappears at top-left over the status text; title "Brush" default leaks if a tool has no name; closing a panel silently changes the tool (could surprise).

### 7b. Viewport overlay HUD
- Kind: viewport overlay panel (`ui::Panel` class `panel`, padding 8, 260dp wide, top-right). Unused by any current provider (experimental slot); would overlap FPS and the view gizmo.

### 7c. Docked tool tab
- Kind: bottom dock tab id "tool:<Category or DisplayName>". Unused now (all providers return Float), path kept as default.
- Problems: three placement mechanisms for one concept, two unused; the comment in `ScenePage.cppm` still describes a persistent "Brush" dock tab that no longer exists.

## 8. Bottom dock

- Kind: panel (`ui::toolkit::BottomDock`), under the viewport only. Hidden entirely when collapsed (`SetHideWhenCollapsed(true)`).
- Tabs: "Animation" (`PropertyAnimationPanel`, editor.propertyanimation, always present); plus a docked tool tab when a Dock-placed tool is active (none today).
- Opened by: toolbar "Animation" toggle, action `scene.view.animationPanel` ("Scene/Animation Panel"), opening a PropertyAnimationClipAsset while this page is visible (claimed by an open-asset interceptor and auto-binds to the primary selection).
- Animation panel's "Bind..." opens the Entity Picker (section 12); a Clear result means "keep binding".
- Problems: a tab strip for a single tab; hiding Animation either switches to the tool tab or collapses the whole dock (state-dependent toggle meaning).

## 9. Hierarchy panel

- Kind: panel. Files: `Editor.Scene/HierarchyView.cppm`, `HierarchyViewImpl.cpp`.
- Where: left pane of the scene page (ratio 0.2). One per page.
- Purpose: browse and restructure the scene's entity tree; select; rename; create/delete; prefab flows.
- Regions: column (padding 6/4, spacing 4): header row [ "+" Button | Filter `EditText` placeholder "Filter..." ] over a `DraggableTreeView` (row height 22, rows are `EditableLabel` at 12pt, horizontal scroll for deep trees).
- Row rendering: name or "(unnamed)"; prefab instance members blue (0.45,0.72,1.0); effectively inactive entities at 45% alpha. No icons, no visibility/lock toggles, no component badges.
- Commands:
  - "+" creates root entity "Entity" (calls `CreateEntity` directly, not through the action).
  - Click selects (single), double-click frames in viewport, slow second click renames, F2 renames, Delete key deletes (direct `DestroyEntity`), arrow keys move selection (synced).
  - Drag a row into another = reparent; drag to a row edge = reorder (cycle-guarded). A dragged row carries the entity id so inspector entity slots accept it.
  - Filter: name substring or whole guid; a whole guid selects, expands ancestors and scrolls to it.
- Context menu on a row (selects it first): Create Child | Rename | Copy ID | --- | Duplicate | Create Prefab from Selection | Spawn Prefab as Child | Spawn Prefab... | [--- | Apply to Prefab | Revert Instance (only when the row is in a prefab instance)] | --- | Copy | Paste as Child | --- | Delete.
- Context menu on empty space: Create Entity | Spawn Prefab... | Paste.
- States: collapse state remembered by guid across rebuilds; new entities expanded. No empty-state hint for an empty scene. During Simulate the "+" and Delete key still call edit methods directly (the locked stack refuses, no feedback); action-backed menu items are disabled.
- Problems:
  - Single selection only: no Shift/Ctrl multi-select in the tree, so a marquee multi-selection shows just the primary row highlighted.
  - No drop of assets (prefab/mesh) to instantiate or parent.
  - Two paths for create/delete (header button and Delete key bypass the action registry, menu uses actions): different enable logic, no Simulate feedback.
  - Filter has no clear button and no match count; placeholder differs from other pickers ("Filter..." vs "Search components...").
  - Header "+" is a bare text button, not an icon like the inspector's.
  - Rename vs frame on the same row by click timing is fragile.

## 10. Inspector

- Kind: panel. Files: `Editor.Scene/InspectorView.cppm`, `InspectorViewImpl.cpp` (3268 lines).
- Where: right pane of the scene page.
- Purpose: edit the selected entity (name, active, transform, every component's reflected fields, bespoke lists) and, in a second tab, the scene's settings blocks.
- Regions: `TabView` with non-closable tabs "Entity" and "Scene". Auto-switches to Entity on a new selection.

### 10a. Entity tab (padding 8/6)
- Empty: label "Select an entity to inspect." (12pt); Add Component hidden.
- `PropertyGrid` categories in order:
  - "Entity": Name (StringEditor), Active (BoolEditor).
  - "Transform": Position (Float3, step 0.1), Rotation (Float3 Euler degrees, step 1, -360..360), Scale (Float3). No header actions (no reset).
  - One category per component present, named by `displayName` attribute or prettified type minus "Component"; unreflected types show the serialization id or "(unreflected component)".
- Component header actions (right aligned icons 18px): Copy (`icons.copy`, tooltip "Copy component"), Remove (`icons.remove`, "Remove component"). Clicking elsewhere on the header collapses.
- Row editors by reflected type: enum -> EnumEditor; f32 -> RangeEditor (with "range" attribute) or FloatEditor; bool; all integer widths -> IntEditor; String -> StringEditor or bone dropdown ("boneName" attribute: "(none)", bones, "<name> (not in the skeleton)"); Float2/3/4; Color; Ref<T> -> `ResourceRefEditor` filtered by asset type (mesh, material, skeleton, clip, nav zone, anim graph, property anim clip, texture/render texture, particle, collision shape, physical material, heightfield, terrain, vegetation mask, audio clip, sound cue, UI document, UI theme); EntityRef -> ResourceRefEditor in entity mode ("(none)", "(missing)") with picker button opening Entity Picker and accepting hierarchy drops; containers -> `ContainerListEditor` (add, per-slot pick, move up/down, remove; material and entity slots accept drops; struct elements get nested sections "<List> N: <name>").
- Attribute conventions: displayName, description (tooltip), range, visibleWhen (row hides by another field).
- Per-component custom sections:
  - RigidBody: NoticeEditor "Shape is Cooked but no collision shape is set - this body has no collider. ..." (refreshed only on rebuild).
  - NavMeshZone: "Bake Navigation" ButtonEditor (section 20).
  - Script: "Behaviors" ContainerListEditor as sections (add, drop ScriptClassAsset, rejection notice); each "Behavior N - <class>" nested section with move/remove header icons, Script picker, Enabled, Update Interval (tooltip "Seconds between onUpdate calls (0 = every frame)"), then rows from the cooked script metadata (Float, Int, Bool, String, Color, Vec3, Entity picker, Asset picker). Unknown or uncooked script = no property rows, no message.
  - Mesh materials: generic reflected list (a bespoke editor exists behind `kUseReflectedMaterialSlots = false`).
  - Prefab members: per-component "Revert to Prefab" / "Revert to Prefab *" ButtonEditor, enabled only when overridden.
- Footer: "Add Component" Button (full width) -> Component Picker; "Paste Component" Button (shown only when the editor clipboard holds a component; confirm if overwriting).

### 10b. Scene tab
- One category per scene system with a reflected settings type (name from displayName or type minus "Settings").
- For profile-capable blocks: a row labelled "Values: this scene's" / "Values: profile '<name>'" with button "Open Profile" (enabled when using a profile), "Make Profile" (enabled when not), "Copy Into Scene" (enabled when using a profile), each with tooltips; success notice "Made profile '<n>'; this scene uses it."
- Field rows via `SettingsRows`: reference fields (picker filtered by producing asset types), enum, f32 (range or float), Color, bool, Float3. Other kinds are silently skipped.
- Physics: "Collision Groups" `CollisionMatrixEditor`: corner add icon (tooltip "Add group", disabled at max), vertical rotated group names across the top (11pt, -90 deg), editable name per row (commit on Enter or blur), checkbox at each crossing (symmetric), remove icon on the last row only (tooltip "Remove this group (the last one)"). Default names "Default", "Group N".
- SceneScriptSettings: Level script property rows from metadata.
- States: read-only reflected props get editors with no setter (look editable? depends on toolkit). During Simulate edits are refused by the locked stack with no visual read-only state.
- Problems:
  - No multi-entity editing (primary only, no "N selected" indication).
  - No search/filter of properties; long components (and the Scene tab) are one long scroll.
  - No component enable/disable checkbox, no reorder, no per-component help or context menu, no Reset; Transform has no header actions while components do.
  - Two row builders (component rows vs SettingsRows) support different type sets (settings lacks int, string, Float2/4), so a settings field of those types vanishes silently.
  - Notices (RigidBody) are stale until the next structural rebuild.
  - Paste Component appears/disappears and shifts layout; Add Component is a big text button rather than a consistent icon action.
  - Simulate state not reflected (fields look editable but writes are refused).
  - "Bake Navigation" runs synchronously in the click with no busy state.
  - Behavior with an uncooked or missing script shows no explanation.

## 11. Component Picker dialog (Add Component)

- Kind: dialog (`ui::Dialog`, title "Add Component", fixed 380x460). File: `Editor.Scene/ComponentPickerDialog.cppm`.
- Opened by: inspector "Add Component".
- Purpose: choose a component type the entity lacks.
- Regions: search EditText "Search components..." (focused on open; Enter adds the first/selected match) | TreeView (row 22): category headings (grey 0.62) with components, all expanded | "No component matches." note (11pt, grey) | description label (11.5pt, word wrap, fixed 34dp) | buttons [Add] [Cancel].
- Interactions: click selects (headings ignored), double-click adds; filter by component or category name. Choices sorted by category then label; unannotated types under "Other".
- Problems: no keyboard arrow/Enter handling documented beyond the filter's Enter; no recently used; no icons per component; fixed size.

## 12. Entity Picker dialog

- Kind: dialog (title "Select entity", min 360x380, max 520x560). File: `Editor.Scene/EntityPickerDialog.cppm`.
- Opened by: inspector EntityRef rows, entity container slots, script Entity properties, animation panel Bind....
- Regions: filter EditText "Filter..." | TreeView (row 20) mirroring the hierarchy, all expanded, current target preselected and scrolled to | buttons [Select] [Clear] [Cancel].
- Interactions: click selects, double-click confirms; whole guid in filter preselects it.
- Problems: title in sentence case ("Select entity") vs title case elsewhere ("Add Component"); row height 20 vs 22 in hierarchy and component picker; no prefab/inactive styling as in the hierarchy; Clear semantics differ by caller (animation panel treats Clear as "keep binding").

## 13. Confirm dialogs and asset picker from the scene page

- "Apply to Prefab" (`Dialog::Confirm`): "Apply '<entity>' to prefab '<asset>'? The prefab asset is rewritten and every instance in open scenes updates to match. This cannot be undone." Opened by action `scene.prefab.apply`.
- "Revert Instance" (`Dialog::Confirm`): "Revert '<entity>' to its prefab? All overrides on this instance are discarded, and any non-prefab entities parented under it are destroyed. This cannot be undone." Action `scene.prefab.revert`.
- "Overwrite Component?" (`Dialog::Confirm`, inspector): "This entity already has a <X> component. Pasting overwrites it (you can undo). Continue?"
- Asset picker (`editor::app::AssetPickerDialog`, types PrefabDocument): Spawn Prefab... / Spawn Prefab as Child.
- Problems: generic OK/Cancel buttons rather than verb buttons ("Apply", "Revert"); the not-undoable operations rely on text alone.

## 14. Notices raised by these pages

Scene page: "Prefab capture failed.", "Prefab asset write failed.", "Created prefab '<n>'.", "The instance's prefab asset no longer exists.", "Apply to Prefab failed.", "Applied to prefab '<n>' (not undoable - the asset changed).", "Instance reverted to its prefab (not undoable).", "Prefab has no content yet (save it once first).", "A prefab cannot contain an instance of itself.", "A prefab needs exactly one root entity - parent everything under a single root, then save.", external modification warning, model import "Prefab '<n>' generated/regenerated (placed instances updated).", "Scene '<n>' generated/regenerated.", tool refusals (UnavailableReason). Status bar: "The launcher will show this view for the project." Game page: "Game: waiting for cook..." (status), "Game: default scene failed to load.", "Game: default input map is not cooked yet.", "Game: default bus layout is not cooked yet.", "Game: startup script asset not found.", "Game: startup script failed to start (see Console).", "Game script error: ...". Inspector: navigation bake results, drop rejection text, "No project is open.", "Assign a Navigation Zone asset to this zone before baking."
- Problems: tone varies (some prefixed "Game:", some sentences, some with "FAILED" in caps on the terrain page); several important states (waiting for cook) go to the global status line, not the page that is waiting.

## 15. Viewport tools framework

- Kind: framework (no own UI). Files: `Editor.ViewportTools/ViewportTools.cppm`, `ViewportToolsImpl.cpp`; panel seam in Editor.App `ToolPanel.cppm`, widget kit `ToolPanelWidgets.cppm` (`MakeToolPanelRoot`, `MakeToolPanelRow`, `SegmentedToggle`, `AddToolPanelFloat`, `AddToolPanelBool`, `AddToolPanelGrid`).
- Contract surfaced to UI: Id, DisplayName (palette label), Category (dropdown grouping), IsAvailable + UnavailableReason (refusal notice), StatusText (top-left viewport text), Draw (DebugDraw overlay), panel Placement (Dock / Float / ViewportOverlay).
- Registered tools: select (default), terrain.sculpt "Sculpt Terrain", terrain.splat "Paint Splat", terrain.hole "Cut Holes" (category Terrain); vegetation.paint "Paint Vegetation", vegetation.scatter "Paint Props" (category Vegetation); spline.edit "Spline" (no category).
- Problems: availability is only checked on click (menus do not grey out unavailable tools); the active tool silently falls back to Select when it becomes unavailable; no tool shortcuts; no tool icons in the palette although EditorIcons has brush icons.

## 16. Select/Transform tool and gizmo

- Files: `Editor.Scene/Tools.cppm`, `ToolsImpl.cpp`, `Gizmo.cppm`, `GizmoImpl.cpp`.
- Purpose: pick, marquee, transform the selection.
- Status text: "Move [World|Local]  (W/E/R mode, X space, Ctrl snap, V vertex snap)", "Rotate [...]  (W/E/R mode, X space, Ctrl snap)", "Scale [Local]  (W/E/R mode, X space, Ctrl snap)".
- Gizmo: translate axes + camera-facing plane quads + centre free move; rotate camera-facing half rings + screen ring + angle guide with degrees; scale axes + centre uniform; handles fade at grazing angles; screen-constant size; scale always local. Drag = one undo group.
- Problems: operates on the primary selection only; snap increments not configurable in UI; space toggle has no effect on scale yet the toggle still flips; X has no action/shortcut entry so it is not discoverable from menus.

## 17. Terrain tools (scene viewport) and floating panels

Files: `Editor.Terrain/Sculpt.cppm`, `SculptImpl.cpp`, `SplatTool.cppm`, `SplatToolImpl.cpp`, `HoleTool.cppm`, `HoleToolImpl.cpp`, panels `ToolPanelsImpl.cpp`.
Opened by: toolbar "Terrain" dropdown. All panels Placement Float (section 7a), title = tool name.

### 17a. Sculpt Terrain
- Purpose: raise/lower/smooth/flatten heightfield.
- Panel: row label "Sculpt mode" (12pt) | SegmentedToggle of 4 SVG icons (brushRaise, brushLower, brushSmooth, brushFlatten; tooltips Raise/Lower/Smooth/Flatten) | PropertyGrid: Radius (0.5..128, step 1), Strength (0..50, step 0.5).
- Keys: 1-4 mode, Shift+wheel radius (mirrored into field), Ctrl+click sets flatten target, LMB stroke.
- Cursor: two circles cyan (orange for Lower).
- Status: "Sculpt [Mode]  radius R  strength S  (1-4 mode, Shift+wheel size, Ctrl+click = flatten target)".
- Unavailable: "Sculpt needs a terrain with a heightfield in the scene."

### 17b. Paint Splat
- Purpose: paint terrain layer weights.
- Panel: (if thumbnails and layers) "Base (erase to reveal)" (11pt) + base swatch 24px (not selectable) | "Paint layer" (12pt) | SegmentedToggle: one swatch per palette layer (live texture thumbnail, fallback `texture` icon; tooltip = albedo asset name or "(no albedo)"/"(missing)"; numbered "0..N-1" when no thumbnails), then "E" (tooltip "Eraser (reveals base)") and "S" ("Smooth (feathers painted seams)") | PropertyGrid: Radius, Strength (0..1, 0.05), Stamp spacing (0.05..1), Airbrush (bool).
- Keys: 1-9 layer, 0 eraser, - smooth, Shift+wheel size.
- Status: "Paint Splat [layer N|ERASER|SMOOTH]  radius R  (1-9 layer, 0 eraser, - smooth, Shift+wheel size)".
- Unavailable: "Paint Splat needs a terrain whose splatmap and heightfield resolve."

### 17c. Cut Holes
- Panel: "Mode" | SegmentedToggle text "Cut" / "Fill" (long tooltips) | Radius.
- Keys: 1 cut, 2 fill, Shift+wheel. Cursor magenta (cut) / green (fill).
- Status: "Cut Holes [Mode]  radius R  (1 cut, 2 fill, Shift+wheel size)".
- Unavailable: "Cut Holes needs a terrain with a heightfield in the scene."

- Problems (terrain tools):
  - Three different mode-picker styles: icons (sculpt), words (hole), single letters "E"/"S" tacked onto the layer row (splat).
  - Strength ranges differ (0..50 sculpt vs 0..1 splat) with no units.
  - Layer numbering: numbered fallback labels and the status text are 0-based, hotkeys start at 1, and the Terrain Editor page names layers "Layer 1..N".
  - Panel scopes layers to the first terrain in the scene while the brush paints whichever terrain is under the cursor: labels can be wrong with several terrains.
  - No undo/redo or stroke feedback in the panel; no falloff control; no brush presets.
  - The panel does not explain the Ctrl+click flatten target or show the current target height.
  - Airbrush row is built with a raw BoolEditor in terrain but `AddToolPanelBool` in vegetation (likely same look, different code path).

## 18. Vegetation tools and floating panels

Files: `Editor.Vegetation/VegetationPaintTool*.c*`, `VegetationScatterTool*.c*`, `VegetationPick*.c*`, panels `VegetationToolPanelsImpl.cpp`. Opened from toolbar "Vegetation" dropdown; Float placement.

### 18a. Paint Vegetation (mask painting)
- Panel: "Mask plane" | SegmentedToggle of plane numbers "0..N-1" (4 when no mask resolved; tooltip "The plane the brush works on (a procedural layer with Mask placement names it)") | "Grows:  0: Grass, Flowers  1: -" line (11pt) or "Grows: no procedural layer reads a plane yet" | "Mode" | Paint / Erase / Smooth (tooltips) | Radius, Strength, Stamp spacing, Airbrush.
- Keys: 1-9 plane, 0 erase, - smooth, Shift+wheel.
- Status: "Paint Vegetation [<mode> plane N]  radius R  (1-9 plane, 0 erase, - smooth, Shift+wheel size)".
- Unavailable: "Paint Vegetation needs a Terrain Vegetation component with a Mask asset, ...".

### 18b. Paint Props (scatter)
- Panel: "Prop layer" | empty note "No prop layers here - add one to the Terrain Vegetation component" | SegmentedToggle of layer names ("<name> (no mesh)" when unresolved) | "Mode" | Paint / Erase | Radius, Density /m2 (0..64), Strength, Spacing (x radius) (0..8).
- Keys: 1-9 layer, 0 erase, Shift+wheel. Cursor orange / white (erase).
- Status: "Paint Props [<mode> layer N]  radius R  (1-9 layer, 0 erase, Shift+wheel size)", plus "- layer N has no mesh: props place but nothing draws".

- Problems: the file header still says "E = eraser, S = smooth" letters (the splat style) while the code uses a Mode row, so vegetation and splat now differ; "Grows:" is one long run-on line that will clip in a narrow floating panel; plane labels 0-based, hotkeys 1-based; Paint Props spacing units "x radius" vs splat "Stamp spacing" fraction with the same concept; layer names in Props vs numbers in Paint Vegetation.

## 19. Spline tool

- Files: `Editor.Spline/SplineTool.cppm`, `SplineToolImpl.cpp`. Toolbar toggle "Spline" (lone tool). No panel.
- Purpose: edit SplineComponent control points of the selected entity.
- Interactions: drag point/handle on camera-facing plane, Shift breaks handle pair, Ctrl+click segment inserts, Ctrl over curve with no segment appends (place preview), Delete or X removes hovered point (two-point floor). One undo per gesture.
- Drawing: curve lines cyan, points yellow spheres, hover orange, handles violet, insert/place previews green.
- Status: "drag point/handle (Shift: break pair) | Ctrl+click segment: insert | Del/X: remove".
- Unavailable: "Edit Spline needs a selected entity with a Spline component."
- Problems: name mismatch ("Spline" vs "Edit Spline"); status text style (lowercase, pipes, no tool name/brackets) differs from every other tool; no panel for closed/open, point list, tangent mode or numeric entry; X reused (gizmo space elsewhere).

## 20. Navigation bake

- Files: `Editor.Navigation/NavigationBake.cppm`, `NavigationBakeImpl.cpp`; UI in inspector (`InspectorViewImpl.cpp` ~1132).
- Kind: inline widget (ButtonEditor row "Bake Navigation" in the Nav Mesh Zone component section). No floating panel.
- Purpose: bake the zone's static geometry with Recast into its NavigationZoneAsset.
- Results (notices): success "Navigation baked into the zone's asset; the scene is unchanged. Cook to apply."; empty "No static geometry inside the zone box. Check ..."; degenerate "Collected N triangle(s) but Recast produced no walkable surface. Try ...". Also "Assign a Navigation Zone asset to this zone before baking.", "No project is open."
- Drawing: NavMeshZone gizmo (zone box) when selected.
- Problems: synchronous bake on the UI thread with no busy indicator or cancel; result only as a toast; no navmesh preview overlay toggle in Overlays; requires a cook to see it; partial rebake (`RebakeNavigationZoneRegion`) has no UI.

## 21. Terrain Editor Page (asset page)

- Kind: page. Files: `Editor.Terrain/TerrainEditorPage.cppm`, `TerrainEditorPageImpl.cpp`. Opened by opening a TerrainAsset.
- Purpose: compose a terrain asset (heightfield, weights, base layer, paint layers) and preview the cooked product.
- Layout: `app::PageToolbar` (standard Save/Undo/Redo/Discard) framing a SplitView 0.62: left PreviewViewport (orbit preview, entity "PreviewTerrain" + "Sun"), right ScrollView of fields.
- Fields (top to bottom): label "References" (13pt) | Heightfield ref | Weights ref | when no weights: label "Create weights:" + buttons "512" "1024" "2048" | label "Base layer" | Base albedo / normal / ORM / height refs | PropertyGrid "Paint layers" ContainerListEditor (sections "Layer N": Albedo, Normal, ORM, Height, Mask refs, Tile, remove icon) | PropertyGrid: Cast Shadows, Height blend, Base tile | label "Stats" | stat lines ("Grid: ...", "World: ... m", "Chunks: ...", "Palette layers: N", "Cast shadows: yes/no") or "(not cooked yet - Save to preview)".
- Notices: "Splatmap create FAILED (name in use?)." / "Created splatmap."
- Problems: section headers are plain Labels mixed with PropertyGrid categories (two visual languages on one page); the size buttons stack as bare buttons; brushes are not reachable from here (must open a scene); preview requires Save + cook.

## 22. Game page (PIE)

- Kind: page (dock tab). Files: `Editor.Play/GamePage.cppm`, `GamePageImpl.cpp`, `GameResolution.cppm`, `PieInstancePage.cppm`, `PlayInEditor.cppm`; actions in `Editor.App/ApplicationImpl.cpp` (~3277).
- Opened by: action `game.play` "Play" (menu Game/Play) reuses the primary instance tab; `game.playNewInstance` "Play New Instance" (Game/Play New Instance) opens an extra tab with its own GameInstance (pie ids `game-page`, `game-page-1`, ...). Both enabled only with a project.
- Purpose: run the project's default scene exactly as the player would, interactively, with script debugging and resolution/phone preview; several instances for multiplayer-like testing.
- Layout: vertical: toolbar (30dp) over a horizontal stage [ game ViewportView (grow, clear 0.248/0.248/0.272, RGBA8 sRGB) | DebuggerPanel (300dp, Gone until shown) ].
- Toolbar left to right: "Play" button | "Pause" toggle | "Stop" button | "Restart" button | "Debugger" toggle | resolution ComboBox (220dp) | "Phone" toggle | status label (13pt) "  Running: <scene>" or "  Stopped".
- Resolution dropdown entries: "Project WxH" or "Project (panel size)"; per export preset "<name> WxH[, <orientation>]" or "<name> (panel size)"; user preview presets "<name> WxH"; "Fit to panel". Refreshed once per second; choice + phone flag saved per project.
- Phone toggle: renders to a 720x1560 portrait target letterboxed in the panel; game turned when its orientation differs.
- Debugger panel: row of plain Buttons "Continue" "Step Into" "Step Over" "Break" (12pt) | status "Debugger: not running / running / paused" | label "Call Stack" + list | label "Locals" + tree with lazy one-level expansion. Auto-opens on a breakpoint.
- Interactions: click viewport to give the game keyboard focus (hover = mouse); while running the viewport captures keys; IME follows game UI; game UI input goes to the focused tab. Game exit request stops the tab.
- States: idle (viewport cleared, no hint); pending (Play pressed, waiting for cook: only the global status says so); running; paused (Pause toggle or debugger break freezes sim); script faulted (notice only); scripted input by agents (PIE MCP tools) with no on-screen indicator.
- Problems:
  - Play/Stop/Pause/Restart are never enabled/disabled by state (RefreshToolbar only sets the label), unlike the scene page's Simulate cluster.
  - Status label never shows Paused, Starting/Waiting for cook, Breakpoint, or Script faulted.
  - Every instance's tab is titled "Game": multiple instances are indistinguishable by title.
  - No empty state ("Press Play") on the idle viewport.
  - Debugger uses plain `ui::Button`s in a Flex row, not toolbar buttons; no F5/F10/F11 shortcuts; plain Labels as section headers.
  - No FPS/stats overlay, no mute toggle per tab (audio focus policy is only in Preferences), no "Simulate vs Play" explanation.
  - Agent-driven scripted input is invisible to the human watching.
  - No icons on Play/Pause/Stop/Restart.

## 23. Preferences contributions

- "Game audio" category: "Hear every Game tab" (description "With several Game tabs running, mix every instance's sound. Off (the default): only the focused tab is heard, ..."). File `GamePageImpl.cpp` ~1536.
- "Navigation" category: "Parallel navmesh bake" ("Bake navmesh tiles across worker threads. The output is byte-identical either way - this only trades bake latency."). File `NavigationBakeImpl.cpp` ~63.
- Related (consumed, defined elsewhere): AutoOrthographicViews (camera), GamePreviewSettings (resolution presets).
- Problems: settings that affect a specific page live only in a global dialog, not reachable from the page.

## 24. Preview viewport substrate

- Kind: inline widget/substrate. Files: `Editor.Preview/PreviewViewport.cppm`, `PreviewViewportImpl.cpp`.
- Used by mesh, clip, skeleton, material, particle, animgraph, collision, terrain asset pages.
- Purpose: a private preview scene + EditorCamera + render loop for asset pages.
- UI: only the ViewportView (clear default 0.349/0.366/0.396, same as scene page, settable). Camera controls same as section 26 (no brush zoom suppression arg).
- Problems: none of the scene viewport's affordances (grid, view gizmo, zoom readout, FPS, Frame, ortho toggle via gizmo) are provided by the substrate, so each asset page either lacks them or reimplements them.

## 25. Thumbnail stage and generators, project thumbnail

- Files: `Editor.Preview/ThumbnailStage.cppm`, `ThumbnailStageImpl.cpp`; `Editor.Scene/AssetThumbnails.cppm`, `AssetThumbnailsImpl.cpp`; `Editor.Terrain/SplatmapThumbnailGenerator.cppm`.
- Kind: offscreen renderer (no UI); output shown by the asset browser and pickers.
- Behavior: 128x128 from a 4x supersampled 512 target, ortho camera along (1,1,1), half extent radius*1.1, one job at a time, Pending retried up to 600 frames.
- Generators: StaticMeshAsset/SkinnedMeshAsset (mesh), MaterialAsset (sphere), PrefabDocument, SceneDocument, SkeletonAsset, ModelManifestAsset, ParticleEffectAsset; SplatmapAsset (terrain).
- Project thumbnail: action `scene.view.setProjectThumbnail` "Set as Project Thumbnail" (Scene menu), and automatic for the default scene; captures without overlays.
- Problems: fixed iso angle (same for every type, e.g. top-down assets read poorly); no failed-thumbnail indicator defined here.

## 26. Editor camera

- Files: `Editor.Camera/EditorCamera.cppm`. No UI.
- Controls: WASD/QE fly only while RMB held or Tab-captured; Shift fast; RMB look; Alt+LMB orbit; MMB pan; wheel dolly 12%; Tab capture, Esc release; frame glide 0.3 s; fov 60 deg; near 0.1, far 1000; ortho height from focus distance; auto-ortho on axis snap.
- Problems: no speed UI or persisted speed; no FOV/clip control; fly speed not scaled to scene size; Shift fast checks only LeftShift.

## 27. Scene actions (menu "Scene", palette, shortcuts)

File `Editor.Scene/SceneActions.cppm`. Menu paths and shortcuts:
- Scene/Simulate/Play, Pause (toggle), Stop.
- Scene/Gizmo/Translate (W), Rotate (E), Scale (R), World Space (toggle), Vertex Snap (toggle).
- Scene/Frame Selection (F), Scene/Orthographic (Keypad5, alt O), Scene/Set as Project Thumbnail, Scene/Entity Markers (toggle), Scene/Animation Panel (toggle, label "Animation").
- Scene/Entity/Create Entity, Create Child, Duplicate (Ctrl+D), Delete (Delete), Copy, Paste, Paste as Child, Create Prefab from Selection, Spawn Prefab..., Spawn Prefab as Child.
- Scene/Prefab/Apply to Prefab, Revert Instance.
- Entity actions disabled while simulating (EditablePage).
- Problems: no Ctrl+C/Ctrl+V on Copy/Paste; no shortcut for Create Entity; menu label "Animation" vs path "Animation Panel"; Overlays/Post/Debug/tool choices are not actions so they are absent from the menu and command palette.

## 28. Cross-cutting inconsistencies (notes for the redesign)

1. Toolbar construction: scene page uses a bare `ui::toolkit::Toolbar` (no Save/Undo/Redo), Terrain page uses `app::PageToolbar` (standard actions), Game page uses a bare Toolbar, debugger uses plain `ui::Button`s in a Flex row.
2. Icon use: TRS icon-only; Space and Overlays icon+text; everything else text-only; no icons for Play/Pause/Stop on either page; brush icons only in the Sculpt panel.
3. Run controls: Scene "Play/Pause/Stop" (Simulate, state-driven enablement, coloured SIMULATING/PAUSED label) vs Game "Play/Pause/Stop/Restart" (never disabled, label "Running: X"/"Stopped", no paused state). Same word "Play" for Simulate and for Game.
4. Checkable menus fake checks with "[x] " text; toolbar uses real toggles; Post menu uses negative "No X" semantics.
5. Persistence: Overlays persisted per scene; Post and Debug view ephemeral; camera preview pin ephemeral; floating panel position not persisted; Game resolution persisted per project.
6. Viewport text overlays use the DebugDraw bitmap font; everything else uses the UI font/theme; overlay positions collide (status text vs floating panel at top-left; FPS, view gizmo, and the HUD target at top-right).
7. Tool status text format differs per tool (bracketed state + hotkey hints, vs spline's lowercase pipe list), and hints duplicate what panels show.
8. Mode pickers in tool panels: icons (sculpt), words (hole, vegetation), letters "E"/"S" (splat).
9. Numbering: splat/vegetation labels and status 0-based, hotkeys 1-based, terrain page "Layer 1..N".
10. Section headers: inspector PropertyGrid categories with header icon actions; Terrain page plain Labels; debugger plain Labels; component picker grey headings.
11. Pickers: "Filter..." (hierarchy, entity picker) vs "Search components..."; dialog titles "Add Component" vs "Select entity"; row heights 22/22/20.
12. Empty states: inspector has text; component picker has "No component matches."; Paint Props has a "No prop layers" note; hierarchy, Game viewport, camera preview, Debug view list have none.
13. Busy states: Game "waiting for cook" only in global status; Nav bake synchronous with no indicator; thumbnails pending invisible here.
14. Disabled reasons: tools refuse after click with a notice; actions just grey out with no reason; Game buttons never grey out.
15. Create/delete paths: hierarchy "+" and Delete key bypass actions; menus use actions; Simulate lock gives feedback only for actions.
16. Selection model: viewport multi-select, hierarchy single-select, gizmo/inspector/entity actions primary-only.
17. Tool panel placement: three targets (Dock, Float, Overlay), only Float used; stale comments describe a "Brush" dock tab.
18. Settings rows vs component rows support different value types.
19. Confirm dialogs use generic OK/Cancel.
20. Camera preview Pin is a plain Button; preview has no title.

## 29. Counts by kind

| Kind | Count | Items |
|---|---|---|
| Page | 3 | Scene Editor Page (also prefab), Terrain Editor Page, Game page (N instances) |
| Panel | 5 | Hierarchy, Inspector (Entity + Scene tabs), Bottom dock (Animation host), Debugger panel, Viewport overlay HUD target |
| Toolbar | 3 | Scene viewport toolbar, Game toolbar, Terrain PageToolbar |
| Viewport overlay | 13 | Grid, origin axes, entity markers, selection box, transform gizmo, marquee, tool status text, FPS, zoom readout, view gizmo, component gizmos, brush/spline cursors, camera preview |
| Floating tool panel | 6 | FloatingPanel host + Sculpt, Paint Splat, Cut Holes, Paint Vegetation, Paint Props panels |
| Dialog | 6 | Component Picker, Entity Picker, Apply to Prefab confirm, Revert Instance confirm, Overwrite Component confirm, Prefab asset picker (shared) |
| Popup / dropdown | 6 | Overlays, Post, Debug view, Terrain tool menu, Vegetation tool menu, Game resolution combo |
| Context menu | 2 | Hierarchy row, hierarchy empty space |
| Inline widget | 8 | Collision matrix, Notice row, Bake Navigation button, Settings profile rows, Script behaviors list, Revert to Prefab row, Paste Component button, Phone toggle |
| Substrate / no-UI | 4 | PreviewViewport, ThumbnailStage + generators, ViewportTools framework, EditorCamera |
| Preferences contributions | 2 | Game audio, Navigation |
