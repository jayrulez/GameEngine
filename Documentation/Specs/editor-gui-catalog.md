# The editor's UI catalog

Status: DRAFT (2026-10-23), the input to the design in [editor-gui.md](editor-gui.md) phase X0.

Everything the editor shows, what each piece is for, and what is wrong with it today, so the new
editor is designed as a whole: each recurring piece designed once and used everywhere. We are not
copying today's look or flows; the catalog says what each piece must DO, and the problems it lists
are what the design must fix.

The detail lives in four documents, read from the code on 2026-10-23 (master and vg at 4fe177db2,
nothing run):

| Document | Covers |
|---|---|
| [frame.md](editor-gui-catalog/frame.md) | The frame: main window, project manager, splash, menu bar and every menu, status bar, toasts, the Project, Assets, Console and Profiler panels, the page host and page toolbar, context menus, every dialog, shared inline widgets, MCP surfaces |
| [scene.md](editor-gui-catalog/scene.md) | The scene page: viewport, toolbar and its popups, overlays, camera preview, hierarchy, inspector, pickers, tool panels (terrain, vegetation, spline), navigation bake, the terrain page, the Game tab, preview viewports, the editor camera, scene actions |
| [pages.md](editor-gui-catalog/pages.md) | Every asset page (22 classes, 25 asset types) and the in-scene property animation panel, their archetypes, a comparison matrix |
| [fields.md](editor-gui-catalog/fields.md) | Every way a value is edited: property editors, slots, list editors, reflection hints, hand-built fields (with file:line), undo behaviour, the new GUI's equivalents and gaps |

Each entry there gives the piece's purpose, files, how it opens, regions and controls, commands,
states, interactions, drawing and icons, problems and inconsistencies.

## 1. The inventory

### 1.1 Frames and window chrome

| Piece | Detail |
|---|---|
| Main window (menu bar, dock, status bar, toasts) | frame.md 1.1 |
| Project manager (launcher: recent projects list and grid, new, open) | frame.md 1.2 |
| Splash (startup progress) | frame.md 1.3 |
| Document tabs (one per page, dirty marker, active ring) | frame.md 1.1 |
| Menu bar: File, Edit, Project, Build, Game, View, Scene, Tools, Help | frame.md 2 |
| Status bar | frame.md 3.1 |
| Toasts | frame.md 3.2 |
| Command palette | frame.md 6.1 |

### 1.2 Global panels

| Panel | Detail |
|---|---|
| Project page (overview, start, scenes, settings summary) and the no-project welcome | frame.md 4.1, 4.2 |
| Assets browser (group tree, breadcrumb, list and grid, filter, badges, thumbnails) | frame.md 4.3 |
| Console (level chips, filter, rows) | frame.md 4.4 |
| Profiler (frame strip, timeline, scope table, counters) | frame.md 4.5 |

### 1.3 The scene page

| Piece | Detail |
|---|---|
| Page layout (viewport, hierarchy, inspector, bottom dock) | scene.md 1, 8 |
| Viewport toolbar and its popups (overlays, post, debug view, tools) | scene.md 2, 3 |
| Viewport, its gestures, and 13 overlays (grid, gizmos, marquee, status text, FPS, zoom readout, view gizmo, brush and spline cursors) | scene.md 4, 5, 16 |
| Camera preview | scene.md 6 |
| Hierarchy (tree, reorder, context menus) | scene.md 9 |
| Inspector: entity tab (component sections, add component, prefab override), scene tab (settings) | scene.md 10 |
| Component picker, entity picker, confirms | scene.md 11 to 13 |
| Tool panels: sculpt, paint splat, cut holes, paint vegetation, paint props, spline | scene.md 7, 17 to 19 |
| Navigation bake (a row in the Nav Mesh Zone component) | scene.md 20 |
| Debugger strip, simulate transport | scene.md 2, 22 |

### 1.4 Play

| Piece | Detail |
|---|---|
| Game tab (toolbar, resolution, phone toggle, debugger, several instances) | scene.md 22 |

### 1.5 Asset pages, by archetype

| Archetype | Pages | Detail |
|---|---|---|
| A. 3D preview and inspector | Material, Settings Profile, Mesh, Collision Shape, Animation Clip | pages.md 1.1, 5, 15, 16, 17 |
| B. Outline, preview and inspector | Particle Effect, Skeleton, Animation Graph (with a node canvas) | pages.md 1.2, 1.3, 2 |
| C. 2D image and inspector | Texture, Image, Font, Heightfield | pages.md 10 to 13 |
| D. Code and live preview | Shader, Script (with its API browser), GUI document and sheet, UI document and theme | pages.md 6 to 9 |
| E. Form or list | Generic form, Sound Cue, Audio Bus Layout, Input Map | pages.md 4.2, 4.3, 14, 18 |
| F. Audition strip | Audio Clip (waveform) | pages.md 4.1 |
| G. Embedded panel | Property Animation (dopesheet, tracks, curve) in the scene page | pages.md 3 |
| Terrain asset page | Terrain | scene.md 21 |

The UI document and theme pages (Editor.GameUI) stay with the old editor (ruling); listed for
completeness.

### 1.6 Dialogs (about 22)

Command palette, asset create, asset picker, group picker, path picker, confirm (two kinds),
export, import (single and batch), preferences (7 tabs), project settings (5 tabs), export
templates, about, save as, unsaved changes (three variants), new project, the project's version
gate, component picker, entity picker (frame.md 6, scene.md 11 to 13).

### 1.7 Context menus and popups

Assets: instance row, group row, background and tree. Console. Project card. List editor add
menu. Hierarchy: row and empty space. Viewport toolbar popups (frame.md 5, scene.md 3, 9).

### 1.8 Value editing

11 old toolkit editors, 6 editor-defined editors, about 12 slot and field widgets, 21 grids, about
80 hand-built fields, 22 reflection hint keys (fields.md 1 to 7).

## 2. The design set: what is designed once

Every recurring piece below is designed once in X0, as a component with its states, and every
surface in section 1 is built from them. Each lists where it is used and what it must handle,
drawn from the problems in the detail documents.

### 2.1 Frame

- **Window title**: project, active page, unsaved marker (today fixed "AssiduousEngine").
- **Menu bar and menus**: shortcuts shown, check marks for toggles, icons where an action declares
  one, a reason on a disabled item, rebindable menu keys. One vocabulary for "New" and "Create".
- **Document tabs**: distinct titles (several Game tabs all read "Game"), dirty marker, a tab
  context menu (close, close others, reveal in assets, copy path), keys to close and cycle.
- **Status bar**: segments, not one overwritten line: the last message (with a history),
  background jobs with progress and cancel, the agent (MCP) indicator, the active page.
- **Toasts**: severity, a capped and grouped stack, an action ("Show in Console", "Open Folder"),
  placed so they do not cover the bottom panels the user is reading.
- **Jobs**: one pattern for long work (cook, export, native build, template install, model
  prepare, navigation bake, font bake): progress, cancel, result.
- **Panels reopenable** from the View menu (Console and Assets today only come back by Reset
  Layout).

### 2.2 Panel and page chrome

- **Panel header**: title, panel toolbar, overflow.
- **Page toolbar**: one component for every page (today three ways): icon with tooltip naming the
  action and its key; the standard groups (save, undo and redo, discard) and the page's own.
- **Transport**: one set (play, pause, stop, restart, loop, speed) for every preview, the
  simulate controls and the Game tab, with states that enable and disable alike and a state label
  (playing, paused, waiting for cook, stopped at a breakpoint, faulted). "Play" names one thing.
- **Page layouts**: the archetypes A to G as templates with fixed placement (where the preview
  sits, where the inspector sits), splitters remembered per page type.
- **Problems list**: one place a page shows its errors and warnings, each clickable to its
  source (today a status line, a fixed error box, the first error only, or the log).
- **Empty, loading and error states**: one pattern, used by every list, tree, viewport, picker and
  page (today mostly a blank area).

### 2.3 Lists, trees and badges

- **Search field**: one component (icon, clear, scope, arrows and Enter into the list, Ctrl+F to
  focus); today five placeholders and two looks.
- **Group tree**: one (today four copies with different rows), with folder icons, counts and the
  preselection shown.
- **Asset row and tile**: type icon, name, type, badges; list and grid; thumbnails.
- **Badges**: cook state, favourite, always export, dirty, prefab override, warning; one look in
  every list and picker.
- **Selection**: multi-select where the data allows it, and every action that takes a selection
  honours all of it (hierarchy, inspector and gizmo act on one entity today).
- **Inline rename, delete, reorder, drag and drop**: one behaviour and one drag ghost (icon and
  name, not a grey box), drop indicators, moving assets between groups.

### 2.4 Dialogs and forms

- **Dialog frame**: title, body, footer with one primary verb button (Enter), a secondary, a
  destructive style; Escape cancels; resizable where content grows.
- **Commit model**: one rule (staged with Save and Cancel, or live with undo), never mixed in one
  dialog (Preferences' module checkboxes write at once today; Export saves on its own).
- **Confirm**: verbs, not OK; destructive style; the affected items listed; one "Unsaved changes"
  dialog listing the pages (today three hand-built variants).
- **Form layout**: one label column, help text per field, validation shown live beside the field
  with the primary button disabled and saying why.
- **Pickers**: one picker for assets, groups, paths, entities and components: inline search,
  the current value preselected, recents and favourites, an empty state that offers to create,
  keyboard throughout.

### 2.5 Values (the property grid)

The new grid (GUI.Toolkit) is the one grid; five reflection-to-widget mappings become one.

- **Row anatomy**: label (resizable column), editor, changed-from-default and prefab override
  markers, revert, read-only look (today read-only fields look editable and snap back),
  validation message, units, range shown.
- **Editors the new grid lacks** (fields.md 8): asset reference slot (pick, drop, clear, open,
  reveal, thumbnail), entity reference, guid, file path with browse, slider and min-max range,
  button and notice rows, read-only info row, layer mask matrix, bone picker (choices supplied at
  run time), key binding capture, segmented choice, multiline text.
- **One way per value type**: angles in degrees with a unit everywhere (four presentations today,
  some raw radians); one rule for precision and step; enums by readable name; HDR colour as one
  row; one key binding control (two today); empty references spelled one way (six today).
- **Undo**: a gesture is one step (separate drags merge today).

### 2.6 Viewports

- **HUD layout**: fixed zones for the toolbar, tool status, stats, the view gizmo, the camera
  preview and the tool panel, so nothing collides (they overlap in the corners today); viewport
  text in the GUI's type, not the debug font.
- **Viewport context menu**, asset drop into the viewport and the hierarchy, snap and grid and
  camera speed controls (missing today).
- **Tool panel**: one chrome (tool name, help, close), one mode picker (icons today in one, words
  in two, letters in one), units and ranges alike, one status line style, numbering from 1.
- **Preview viewport**: camera hint, frame and reset, background and lighting, a stats overlay
  convention, preview-only settings kept apart from the asset's data.
- **Image viewer** (texture, image, font, heightfield): zoom, pan, channels, alpha checkerboard,
  mip, exposure, pixel readout.

### 2.7 Words and icons

- **Message style**: sentence case, no shouted FAILED or NOT, "the Console" with a link, one
  phrasing per outcome.
- **Naming**: one name per action and object ("Rename" for every rename, "AssiduousEngine" in
  About), title case for headings.
- **Icons**: one meaning per icon (trash means two things today, the close X three); a type icon
  for every asset type (most share "unknown" today) and component; action icons shown where the
  action appears. The icon list is drawn up in X0 from the action registry and the type registry.

### 2.8 Keyboard

Enter and Escape in every dialog; F2 rename and Delete in every list and tree; Ctrl+F to the
search; arrows in lists and pickers; page shortcuts for the transport, frame selection and adding
or removing a list item; tab cycling. Menus and the palette show every chord.

## 3. Found in the old editor: in scope (ruling)

Read from the code, not reproduced. Fixing these is in scope (the user, 2026-10-23): the new editor
must not have them, and each lands with a test in the phase that builds its piece. Where the new
GUI's piece already behaves (the grid's gestures), the test proves it.

| Defect | Fixed in | The test |
|---|---|---|
| Particle gradient stop colours cannot be edited: the gradient editor's colour request has no handler (pages.md 23, fields.md 9) | X6 (Particles) on GradientView | a stop's colour edited through the page changes the asset, one undo step |
| Consecutive edits of one field merge into one undo step (fields.md 5) | X4 (the inspector on the grid) | two separate drags of one field are two undo steps; one drag is one |
| Integer widths are not enforced: 300 typed into a u8 wraps (fields.md 9) | X4 | a u8 field refuses or clamps 300 and says so, never wraps |
| After Return in a text field, further typing is overwritten by the refresh and the commit on leaving is skipped (fields.md 3.5) | X4 | type, Return, type again, leave: the last text is committed |
| Collision Shape edits bypass undo while its Undo and Redo show (pages.md 17) | X6 (Physics) | every edit undoes and redoes |
| Project Settings shows the running editor's version as the project's engine version (frame.md 6.11) | X3 (project settings) | the field shows the project's stamp |

## 4. Features found missing

The surveys also list what the editor does not do at all (asset drag into the viewport, a
viewport context menu, property search in the inspector, component enable toggles, Save All, a
tab context menu, console timestamps, and more, in each document's problems). Which of these the
new editor gains, and when, is decided per page as its design is written; none is promised by
this catalog.
