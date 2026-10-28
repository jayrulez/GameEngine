# The editor on the GUI: migration and design

Status: DRAFT (2026-10-23). The user's rulings of 2026-10-23 are in section 9; the questions
still open are marked there.

The editor moves from foundation.ui to the GUI (foundation.gui and its satellites), and gets a
designed look while it moves: one stylesheet over design tokens, an icon set, motion, density and
a light theme. The design comes first and is judged by the user before any page moves, so every
page is touched once.

## 1. Where we start (surveyed 2026-10-23)

**The editor.** 93,744 lines of source and 26,945 of tests in `Code/Editor`. 29 libraries import
foundation.ui; six import none (Mcp, Project, Camera, Navigation, Spline, ViewportTools). Every
layout is built in code from FlexLayout, Label and Button; the editor has no markup. The largest:

| Library | Lines | What it uses most |
|---|---|---|
| Editor.App (the shell, dialogs, assets browser, console, profiler, MCP UI tools) | 22,092 | FlexLayout, Label, Dialog, EditText, DockManager, MenuBar, StatusBar, Toolbar, ToastHost |
| Editor.Scene (the scene page, hierarchy, inspector, tool panels) | 19,974 | PropertyGrid and editors, TreeView, DraggableTreeView, FloatingPanel, ViewportView |
| Editor.Animation, Particles, PropertyAnimation | 10,000 together | PropertyGrid, NodeGraphCanvas, CurveCanvas, GradientEditor, Timeline |
| Editor.Play, Preview | 4,900 | ViewportView, Toolbar |
| 20 asset page libraries | 200 to 3,300 each | PropertyGrid, SplitView, ImageView, CodeEditView |

What the editor needs from a UI, and where the GUI has it:

| The editor uses (foundation.ui) | The GUI has |
|---|---|
| DockManager, floating OS windows (RuntimeDockableWindowHost) | GUI.Docking: DockLayout (layout as data, versioned document), DockSpace, DockHostWindows |
| PropertyGrid, PropertyEditor | GUI.Toolkit T1 to T3: virtualised rows, editors by hint then kind, several objects and mixed values, lists, nested objects, revert |
| CurveCanvas, GradientEditor, Timeline, NodeGraphCanvas | GUI.Toolkit T5, T6: CurveView, GradientView, Dopesheet, GraphView |
| Toolbar, StatusBar, BreadcrumbBar, MenuBar | ToolBar (from command ids), StatusBar, BreadcrumbBar, MenuBar |
| CodeEditView | GUI.Code (C0 to C8) |
| ViewportView and InputSurface | GUI.Viewport (IHostedContent), GuiHost's input gate |
| TreeView, DraggableTreeView, ListView | TreeView, ListView, TableView (virtualised), drag and drop |
| Dialog, ContextMenu, ToastHost | Window (modal), MessageBox, Menu, ToastHost, popups |
| UIContext tests with synthetic events | GUI.Testing: Finder (selectors, text), Tester (clock, clicks, drags, keys) |

Not in the GUI yet, and needed (checked against the code and its tests, 2026-10-24; what was
listed here before is mostly there: a menu item shows its command's keys and follows a
rebinding, a menu's arrows and Escape are bindings in the `menu` context, lists, trees and
combo boxes type ahead, and Alt access keys work in buttons, menu bars and menus): a menu's
Return and Space as a binding, and type-ahead in an open menu; a binding replaced rather than
added to (a rebinding leaves the old key working); keyboard capture for a game in a panel
(GUI.Viewport takes no keys of its own: Tab and Escape still act); dialog buttons with a role
(primary, destructive, cancel) and verbs in markup; an icon in a tree row; dialogs that leave
through `:leaving` (popups are removed at once; menus, lists and tooltips stay so, ruling 14); a
rule for vector fields in a narrow inspector (left as it is for now, ruling 15);
an access key on a field's label. A float docks back whole by its title within the window;
one in its own OS window does not. Not needed: the press-drag-release menu bar (menus
keep opening on release, ruling); popups as OS windows (foundation.ui has none either: its popups
are placed in their root and clamped to the window, `ContextMenu.cppm`, `PopupPositioner`).
Platform accessibility adapters stay out of scope.

**The editor's look today.** One hard-wired theme: RoundedDark with the GraphiteOrange palette
(11 colours), a `property-field` override and close-button drawables set in code
(ApplicationImpl.cpp 267 to 319). Roboto and DejaVu Sans Mono, MSDF atlases cached on disk. A UI
scale setting (0.5 to 2.0). Icons: 34 inline SVG strings in EditorIcons.cppm, 6 in TemplateIcons,
10 in foundation.ui's ThemeIcons, all 24x24 and single fill, baked to an atlas. Motion: 120 ms
colour fades from the theme sheet, a toast's expiry, the zoom readout's fade. No light theme, no
density setting, no accent setting.

**The GUI's styling.** Tokens on `:root` in three layers (base, derived by `color-mix`, per
widget with fallbacks), light, high contrast and reduced motion as `@media` blocks;
`SetMediaContext` sets the colour scheme and reduced motion an application chooses; satellite
sheets (docking, toolkit, code) written over the tokens; transitions, keyframes, `:entering` and
`:leaving`, springs; SVG drawn at the device size it lands at and tinted by `image-color`.

## 2. What the references do (surveyed 2026-10-23)

- **Tokens in two layers.** A few inputs (base colour, accent, contrast, density, radius) expand
  into many semantic tokens: Godot in C++ (ThemeConfiguration), Zed by hand in JSON. Zed's
  vocabulary is the most reusable: background, surface, elevated surface; `element_*` and
  `ghost_element_*` state sets (background, hover, active, selected, disabled); text and icon
  each with muted, placeholder, disabled, accent; status as triples (colour, background,
  border); elevation levels each with a background and a shadow. LunarSong (a C++ editor on CSS)
  writes the same as CSS custom properties in one `tokens.css`, with contrast ratios in comments.
- **Density is a multiplier with three steps.** Godot: Compact, Default, Spacious (base spacing
  2, 4, 6). Zed: 0.75, 1, 1.25 over a spacing table.
- **Sizes converge.** Base spacing 4 px, radius 3 to 6, icons on a 16 px grid, inspector rows and
  headers 28 px, tabs about 32, UI text 13 to 14 px with 12 small.
- **Icons.** Zed: 301 stroke icons, 16 px, rendered as a mask and tinted (Lucide, ISC). LunarSong:
  377 white stroke icons, tinted, named by type. Godot: about 1,045 filled icons named by class,
  colour-coded by category (3D red, 2D blue, Control green), found up the class chain, remapped
  by a table for the light theme (MIT).
- **Motion is short and optional.** 50, 150, 300 ms; ease-out (quint or quad) to enter; hover
  fades 100 to 200 ms; scrollbars show fast and hide slow. Zed and gpui-ce honour reduced motion
  in the animation layer (snap to the end state), never at call sites. Godot has almost none.
- **Theme files are data.** A light and dark pair chosen by the system, per-token user overrides,
  a watched directory and hot reload (Zed, 100 ms debounce).
- **Fonts.** OFL pairs: Inter and JetBrains Mono (Godot), IBM Plex Sans and Lilex (Zed), Roboto
  and Roboto Mono (LunarSong, Lumix).

## 3. The design language (proposed)

### 3.1 Tokens

One editor sheet, `editor.css`, imports `tokens.css` and is the only place the editor's look is
written. It sets the GUI's existing tokens (so the core widgets and the satellite sheets follow
with no overrides) and adds the editor's own. Three layers, in CSS:

1. **Inputs**, the few a user sets in Preferences: `--base`, `--accent`, `--contrast`,
   `--density` (0.75, 1, 1.25), `--radius-base`, `--ui-font`, `--mono-font`, `--font-size-base`.
2. **Semantic tokens** derived from the inputs with `color-mix` and `calc`, named in Zed's
   vocabulary mapped onto the GUI's names where one exists:
   - surfaces: `--surface-sunken`, `--surface`, `--surface-raised` (the GUI's), plus
     `--surface-overlay` (popups and dialogs), each with an elevation shadow;
   - elements: `--element`, `--element-hover`, `--element-active`, `--element-selected`,
     `--element-disabled`, and the ghost set for flat buttons in bars;
   - text and icons: `--text`, `--text-dim`, `--text-strong` (the GUI's) plus
     `--text-placeholder`, `--text-disabled`, `--icon`, `--icon-dim`, `--icon-accent`;
   - status triples: `--danger`, `--warning`, `--success`, `--info`, each with `-background` and
     `-border`;
   - regions: `--titlebar`, `--toolbar`, `--tabbar`, `--panel`, `--panel-focused-border`,
     `--viewport-frame`, `--drop-target`;
   - spacing `--space-xs` to `--space-xl` as `calc(n * 4px * var(--density))`, row height
     `--row` (28 px at density 1), tab height `--tab` (32 px), icon size `--icon-size` (16 px);
   - motion: `--motion-instant` 50 ms, `--motion-fast` 150 ms, `--motion-slow` 300 ms,
     `--easing-out` (ease-out-quint), `--easing` (the GUI's, for state changes).
3. **Per region rules** in `editor.css` (the shell, a page's header, the inspector), written only
   in semantic tokens, never in literals.

Dark and light both ship with the new look (ruling). They are two blocks of inputs under `@media (prefers-color-scheme)`; the editor sets
the scheme through `SetMediaContext` from Preferences (System, Dark, Light). High contrast is the
same. Every derived token follows, and so do the satellite sheets.

The token list is written out in the systems doc, with each colour pair's contrast ratio.

### 3.2 Icons

- **UI icons**: monochrome SVGs on a 16 px grid (drawn at 16, 20 and 24 by the device size they
  land at), stroke 1.5, round caps, drawn in white and tinted by `image-color` from `--icon`,
  `--icon-dim` or `--icon-accent`. One file each under `Data/Editor/Icons/`, named in kebab case
  (`play.svg`, `folder-open.svg`), referenced from markup and sheets by name.
- **Type icons**: one per asset type and component type, named by the type's reflected name
  (`MeshRendererComponent.svg`), found by asking the type: its own icon, then its base's, then a
  generic one. Where they sit in lists the eye scans (the hierarchy, the assets browser, the
  component picker, the inspector's component headers) they are tinted by a category token
  (render, physics, audio, script, GUI, and so on), so the kind of a row reads before its name;
  in bars and menus they are monochrome like every UI icon. A light theme sets the category
  tokens; no remap table.
- **Source**: all drawn by us (ruling), in one style and grid, kept under the repository's licence.
  The 50 inline SVGs today are replaced, not ported.

### 3.3 Motion

What moves, and nothing else: hover and press colours (`--motion-fast`); popups, menus and
tooltips enter with a 4 px slide and fade (`--motion-fast`, ease-out) and close at once, as a
desktop editor's do (ruling 14); dialogs scale from 98% and fade in and out (`--motion-slow`);
toasts slide in and out; a docking preview
fades; tab reorder and tree expand animate their positions (`--motion-fast`); scrollbars show in
50 ms and hide in 400 ms after idle; a page's busy state pulses. Nothing animates layout on
resize, and the viewport never waits on motion.

Reduced motion (Preferences: System, On, Off) goes through `SetMediaContext` and the sheet's
`@media (prefers-reduced-motion: reduce)` block, which sets every motion token to 0; nothing in
code checks it.

### 3.4 Type and density

Inter and JetBrains Mono (both OFL, ruling), with Noto fallbacks for scripts Inter lacks; the
licence texts beside them and in the credits. UI text 13 px, small 12, headers 14 semibold, code 13. Three densities. The UI scale
setting stays and multiplies everything.

### 3.5 Structure of a page

Every page is a document: `.gml` markup for its static structure and a sheet for anything only it
needs, under `Data/Editor/`, loaded through the editor's resource provider and reloaded when the
file changes (the editor's own development loop). Code binds behaviour to elements by id and
fills dynamic content (rows, trees, the grid). Dialogs are markup. The command palette,
menus and toolbars stay generated from the action registry.

### 3.6 The look is judged first

The accent is new, chosen in X0 (ruling). Phase X0 builds the language as a showcase before any editor code moves: the token sheet, the
icon set, and a static mock of the editor's shell (menu bar, toolbar, dock with a scene page,
hierarchy, inspector, assets, console, status bar, a dialog, a menu, a toast) in markup, run in
the GUISandbox, dark and light, three densities, reduced motion on and off. The user judges it;
the migration starts from the approved mock.

## 4. Migration strategy

**A parallel editor on a branch, rewritten in place (ruling).** The two UIs never run in one
editor, so no page on the old UI is hosted in the new frame. The work is on the branch
`editor-gui`, taken from vg at 4fe177db2; vg gets no more changes (the mirror took its copy there),
and every change, the GUI libraries' included, lands on `editor-gui`. The old editor stays on vg
and master until the swap.

The editor's libraries are rewritten in place, file by file: a file keeps its name and its role,
and a port commit shows its UI moving from foundation.ui to the GUI. The mirror ports the same
commit against its own copy of the same file.

**Parked, not commented out.** A library not yet moved is parked: its source is left exactly as it
is, it and its tests leave the build (its `add_subdirectory` and its link from Tools.Editor), and
Tools.Editor's `Main.cpp` does not register its pages. A port commit moves one library onto the
GUI whole, with its tests, and puts it back in the build and in `Main.cpp`. Editor.App, the shell,
is the one library ported in parts: the files not moved yet (a dialog, the profiler panel) leave
its source list, again unedited. The editor runs at every step, without its parked pages.

**A port commit:**

1. Keeps file names where the role is the same; renames are their own commit.
2. Puts a file's markup and sheet under `Data/Editor/`, named after it (`HierarchyView.cppm`,
   `HierarchyView.gml`, `HierarchyView.css`).
3. Updates the ledger (section 4.1) with the library's state.

Kept in the build throughout, or with its foundation.ui imports removed: Editor.Project, Editor.Mcp,
Editor.Camera, Editor.Navigation, Editor.Spline, Editor.ViewportTools (no foundation.ui today), and
Editor.Core (four files import foundation.ui: Actions for KeyCode, Context for the clipboard,
ThumbnailService, ContextImpl; these move to the GUI's or the platform's types). Gizmos and
overlays drawn through DebugDraw, and the renderer's work in a viewport, carry over unchanged.

Written anew on the GUI: the shell, the dialogs, the panels and every page's UI, each as markup
and a sheet (section 3.5) with code that binds them.

**Settings**: the dock layout moves to DockLayoutDocument; an old layout is migrated where it maps
and replaced by the default where it does not (a version bump).

**MCP**: `layout_get` and `panel_dock` move onto DockLayout (same ids, same positions);
`editor_screenshot` and `action_*` carry over. The guide updates with them.

**The swap**: when the new editor does everything the old one does (every page, every MCP tool,
Integration.Mcp green, the user's playthrough of the sample projects in it), the ledger has
nothing parked but Editor.GameUI, which is deleted, and `editor-gui` is merged when the user says.
Removing foundation.ui and Engine.UI after that is a separate decision for the user (the
coexistence ruling stands).

### 4.1 The ledger

Every editor library and its state on `editor-gui`. Kept by the port commits; the mirror reads it
to follow the same order.

| Library | State | Ported in |
|---|---|---|
| Editor.Project, Editor.Mcp, Editor.Camera, Editor.Navigation, Editor.Spline, Editor.ViewportTools | No UI: stay in the build | |
| Editor.Core | In the build, on the GUI: its shortcuts are the keymap's keystrokes (kept as text in the settings, an old key code read once), its action items a GUI menu, its change notice an Observable, its clipboard and thumbnails the GUI's | X2 |
| Editor.App | In the build in parts: the UI-free files, the page's content a GUI node, the application broken up (window, session, native module, export, asset creation, layout store, MCP host, actions), the dock with the Project, Assets and Console panels (the last two a line each until X4), its layout saved with the project (an old dock's tree read into it), the pages through the dock (opened, closed with the unsaved-changes prompt, the active page, their tabs' marks, the open pages saved), the menu bar and the keys from the actions, the status bar (the message, a job, the agent) and the toasts over the dock, the command palette, Save As and the unsaved-pages gate at exit, Preferences (Appearance: scheme, accent, density, motion, UI scale, axis views; Export; Agent access; Game preview; Shortcuts, a capture per action; a tab per contributed category), the project home in the Project panel and the project manager over the window with no project (New Project, Open Folder, the version check, File > Close Project), the MCP layout tools on the GUI's dock (ruling 16's shape) and the action tools, an unattended run's modal windows refused by the GUI's dispatcher; the Console, asset drag data, the asset slot and the inspector's asset reference, the picker, the path picker and the asset picker (the slot's pick), the group picker and Create Asset, Project Settings, Export Templates and Export, the assets browser (browsing, its verbs, moving, thumbnails, import) (the old files left out deleted at the swap) | X3 (layers 1 to 15), X4 (layers 1 to 7) |
| Tools.Editor | In the build with a trimmed Main: no pages yet | X3 (layer 3) |
| Editor.Scene | In the build in parts: its UI-free half (the edit context and its commands, the gizmos and the select tool, the component gizmos, the grid, the view gizmo, the zoom readout, the view settings, the camera preview's sizing, the capture recorder, entity JSON, loading, settings profiles, the thumbnail generators, the page interface, the scene MCP tools, the actions on the keymap's keystrokes) and its services (the document types, the export's scene seams, a model import's prefab), registered by Tools.Editor; the scene page on the GUI in parts (the page, its viewport, its overlays, its view state, Simulate, the prefab flows, the registration), its bar on the actions, the hierarchy on the GUI's tree, the inspector on the property grid (its first part: the head, Active, Transform, the components' reflected rows, their verbs, Add Component, one undo step an edit, read only while simulating; its second: entity references, a slot over the scene's entities; its third: a component's extras asked of its type, the rigid body's notice, the zone's Bake, the script component's behaviours) (the old picker dialogs deleted at the swap) | X5 (layers 1 to 4, part 1) |
| Editor.Preview | In the build, on the GUI: the thumbnail stage (owned by the editor session per project, staged and rendered by the application, the scene editor's generators registered again); the preview viewport on GUI.Viewport, flown as the scene view is, F framing, one backdrop for every preview | X5 (layer 2), X6 (layer 3) |
| Editor.Play | In the build, on the GUI: the resolutions a Game tab offers, the PIE instance seam, the PIE MCP tools; the Game tab from its markup on GUI.Viewport (the kit's transport, the resolution and the phone, the script debugger beside the game), registered by Tools.Editor; several instances side by side (the Game menu's Play and Play New Instance, the layout tools) and the PIE MCP tools on it | X5 (layer 7, parts 2 to 4) |
| Editor.PropertyAnimation | In the build, on the GUI: the property animation panel under the scene page's viewport (its UI-free half: the clip document with its own history, the clip as the dopesheet's and the curve view's source, the track seeds, the preview, the transport's clock; the panel from its markup, its own Undo, Redo and Save, its keys its own while it holds the focus; on the scene page in a splitter, from the Scene menu and an animator's Edit Clip, its unsaved clip asked about apart from the scene) | X6 (layer 9) |
| Editor.Animation | In the build in parts, on the GUI: what the pages' previews share (the bones drawn, the travelled path, the rig: a skeleton and a skinned mesh picked on the preview's bar); the clip page from its markup (the cooked clip played on the rig with the kit's transport and Travel, the ruler under it scrubbing, the events its markers; the clip's rows its own: Looping, the root motion, the events a list; its facts read only; each edit one undo step); the skeleton page from its markup (the bones a tree with its search beside the bind pose, read from the asset, the chosen bone drawn apart, its facts and the skeleton's read only); the graph as a graph source (states and Any State the nodes, transitions node links, ids kept through edits, a move one step); the graph page from its markup (the layers and parameters as lists, the chosen layer on the graph view, the preview with the kit's transport and the playing state lit, the grid the chosen layer, parameter, state or transition with each row where it applies, the preview's parameters the page's own; each edit one undo step) | X6 (layer 10) |
| Editor.Particles | In the build, on the GUI: the particle effect page from its markup (the systems a tree, each its emitter, initializers, behaviors and sub-emitters, its bar adding a system and removing the chosen system or module; the live preview with the kit's transport, which never writes the effect, a speed of the page's own, the particles alive and the chosen system's emission shape; the chosen system's rows from reflection, each shown where its render mode, emission mode or shape uses it, its modules the grid's lists added by type, a module chosen in the tree opened there, its curves and gradients the toolkit's; each system's sub-emitters, new; each edit one undo step) | X6 (layer 8) |
| Editor.Generic | In the build, on the GUI: the fallback asset page (Discard Changes on its bar, as on every asset page and the scene page, where the old toolbar had it), its serialize-scanned fields a property bag on the grid (an array's fields a folded group, a guid one asset reference, a blob its size), from its markup; on Editor.App's shared asset page parts (the services a page asks for, the XML text format and the reflected edit's undo step, the scene's inspector on them too) | X6 (layer 1) |
| Editor.Image, Editor.Texture, Editor.Heightfield, Editor.Fonts | In the build, on the GUI: the image pages from one markup, each the picture (zoom about the pointer, pan, fit, 1:1, a channel alone, the pixel under the pointer: a heightfield's in metres) beside the grid; on Editor.App's asset page base (the asset's reflected rows, the page's own sections, its facts read only, notices, one undo step an edit, a preset, a followed edit); the texture's presets on its bar, its usage setting its colour space; a font's ramp sizes a row of its own; their thumbnails registered with them | X6 (layer 2) |
| Editor.Geometry, Editor.Physics | In the build, on the GUI: the preview pages from one markup, the preview beside the grid, the preview's own choices on its bar (Editor.App's PreviewPane); the mesh viewer (its stats read only, its preview material kept per mesh, a chain's level held); the collision shape (its outline, its source mesh a slot for what a mesh cooks to, each edit one undo step, no mesh a notice, no Cook now) | X6 (layer 3) |
| Editor.Materials, Editor.Render | In the build, on the GUI, on the preview pages' markup: the material page (its shader's table in three sections: the material's (the shader a reference, the built-in template while there is none, the pipeline state), its properties each by its declared type (a range from the shader, a colour, an HDR colour a colour and an intensity, a vector its parts), its textures each a slot), the preview on a shape or a mesh of the project, kept per material, a stale cook and a missing shader as notices); the settings profile page (the values' rows, the scene's own fields left out, each edit one undo step, previewed on the built-in scene or a project scene picked on the preview's bar, kept per profile) | X6 (layer 4) |
| Editor.Audio | In the build in parts, on the GUI: the audio clip page from its markup (the clip's waveform, a widget of the editor's coloured by the sheet, under the kit's transport, the playback actions over the page, auditioned through the runtime's audio engine with a playhead and a volume of the page's own; the clip's options on the grid, each edit one undo step the audition follows; its source's facts read only; a missing source said in the waveform's place), the sound cue page (how it picks and its pitch and volume ranges, a variant per clip, a clip slot and its weight, and one more to fill, each edit one undo step; the kit's transport playing the variant the runtime's resolver picks, what it picked said beside it; no clip a notice), the audition shared by the audio pages (a clip read from its source, one voice), the bus layout page (the buses a tree beside the chosen bus's rows: a custom bus's name and parent, only the parents that make no loop offered, then its mix, filters, delay and reverb, each a section; a bus added and removed from the tree's bar and a row's menu; a loop said; each edit one undo step), the clip thumbnails | X6 (layer 5, parts 1 to 3) |
| Editor.Input | In the build, on the GUI: the input map page from its markup (the sets and their actions a tree beside the grid, its bar adding a set and removing the chosen row, its search by an action's name or input; the chosen set's rows from reflection, each action opening in place and saying its kind and bindings in a line when closed, a binding's settings shown by its source, the sources by what the action's kind takes; an action chosen in the tree opened there; a binding's input captured by pressing it, the page reading the devices; each edit one undo step; a map the game could not read a notice, Save refusing it) | X6 (layer 5, part 4) |
| The Terrain page; a Vegetation Mask page (new, layer 7) | In the build, on the GUI: the terrain page (the cooked terrain under a sun beside its rows, its paint layers a list of groups by channel, new splat weights from the bar) and the vegetation mask page (a density plane at a time, its options as its cook reads them) | X6 (layer 7) |
| Editor.Script | In the build, on the GUI: the script page from its markup on Editor.App's code pages' base (the source file read into the code editor, coloured by its language's shared definition, checked a moment after each edit as the cook compiles it into the kit's problems list and onto its lines, a problem chosen taking the code there, one history for the code and the page, Save writing the source then cooking, a missing source a problem and the code read only); completion and hover from the API the runtime binds for its language, the API browser (its search over a tree) opened from the bar beside the code, breakpoints in the gutter shared with the Game tabs, a paused run's line and values | X6 (layer 6, parts 1 to 3) |
| Editor.Script.AngelScript, Editor.Script.Luau | Retired: they held only the old code widget's lexer tables; the languages are data now (Data/Assets/code) | X6 (layer 6, part 3) |
| Editor.Shaders | In the build, on the GUI: the shader page from its markup on the code pages' base (the code coloured by the shared HLSL definition, checked against the engine's library a moment after each edit, its file's errors on their lines and an include's in the problems list) beside the preview (a sphere drawn with the code being edited under the page's own shader name, its material laid out by the properties block) over the asset's grid (its name and entry points, each edit one undo step in the code's history); a preview kept on the last version that compiled says so; its old lexer retired | X6 (layer 6, part 4) |
| Editor.GUI | In the build, on the GUI: the GUI document and sheet pages from one markup on the code pages' base (the code coloured by the shared XML or CSS definition, checked as the cook checks it into the problems list and onto its lines) beside the live preview the runtime's GUI draws into a viewport of the page (the game's fonts, theme and sheets); a sheet previewed over markup of the page's own, kept for it in the project's editor settings (never an edit of the asset), its problems named apart, a copy of a document's markup on the bar's choice; markup that does not build keeps the last preview and says so | X6 (layer 6, part 5) |
| Editor.GameUI | Left with the old editor (ruling): never ported, deleted with its tests at the swap | X8 (layer 1) |
| The old editor's files | Deleted at the swap: the files left out of Editor.App's and Editor.Scene's source lists (the old shell, page toolbar, list editor, compact slot, icons, view toggles, list header, category tabs, the component and entity picker dialogs) and their old tests | X8 (layer 1) |

## 5. Phases (proposed)

| Phase | What lands |
|---|---|
| X0 | Designed over the catalog ([editor-gui-catalog.md](editor-gui-catalog.md)): every surface built from its design set. The design language: `tokens.css`, `editor.css`, the icon set (drawn by us), Inter and JetBrains Mono, the new accent, and a static mock of the shell in the GUISandbox, dark and light, three densities, reduced motion. Judged by the user. |
| X1 | GUI gaps the editor needs (1.1's list): dialog buttons with roles and verbs, icons in tree rows, dialogs leaving through `:leaving`, a menu's activation as a binding and type-ahead in menus, a binding replaced on rebind, keyboard capture for a game in a panel, an access key on a field's label. Each in its library with tests, in the GUISandbox. |
| X2 | The editor parked: every UI library out of the build (the ledger), Editor.Core off foundation.ui, the editor's resource provider with sheet and markup reload, the icon registry asking types. |
| X3 | The shell: GuiHost window, menu bar from actions, DockSpace and layout persistence, status bar, toasts, command palette, Preferences (theme, accent, density, motion, scale), the project home. MCP host, actions and layout tools. |
| X4 | Shared editor parts: the asset picker slot, asset drag data, the reflected inspector on the GUI's property grid (the editor's editors registered by hint), the dialogs, the assets browser, console. |
| X5 | The scene page: viewport, hierarchy (tree with reorder), inspector, tool panels, camera preview; play in editor (the Game tab). |
| X6 | Asset pages, in groups: property pages (Generic, Texture, Fonts, Image, Heightfield, Render, Vegetation, Audio, Materials, Geometry, Physics, Input, Terrain); code pages (Script, Shaders, GUI on GUI.Code, with CSS highlighting); curve and timeline pages (Particles, PropertyAnimation, Animation); Preview. |
| X7 | The profiler, a tab in the page area, on a new library Profiler.GUI (5.5). |
| X8 | Parity check and the swap: every MCP tool, Integration.Mcp, the sample projects played in the new editor; the old editor deleted. |
| X9 | Hot reload of user theme overrides, accessibility pass, the token reference in the docs. |

Each phase is one or more layers, one commit each with tests; the editor is run and screenshotted
over MCP before each is reported, and the look is the user's to judge.

### 5.1 X3, the shell, in layers

Mapped from Editor.App on 2026-10-24 (about 22k lines, ApplicationImpl.cpp 3.4k). Editor.App comes
back in parts: a file not ported yet stays out of its source list, unedited. Each layer is one
commit with its tests; the GUI library additions go first and are grouped with the other GUI
commits.

GUI library additions X3 needs (GUI.Docking and the core):
- a close veto (a dirty page asks before it closes): done, a description's CanClose, and CloseNow
  once the user agreed;
- an activation callback (the active page follows the focused stack): done, the controller's
  ActivePanel and SetOnActiveChanged;
- a panel's title and classes changed while it is docked (a dirty marker, the active mark): done,
  the controller's SetTitle and SetTabClasses over a TabPage's tab classes;
- a modal-open interceptor (MCP's unattended actions close the dialogs they open as cancelled;
  foundation.ui's DialogInterceptor): done, on the event dispatcher (Window reaches it, not the
  context), SetModalInterceptor, a refused window answering as its cancel.

The layers:
1. Editor.App back in the build with its UI-free files only (font atlas cache, window capture,
   splash, the MCP host, project operations, the page, window and profiler tools, playback actions,
   property names, the asset filter) and their tests.
2. UIEditorPage's content is a GUI node.
3. The application on GuiHost, and broken up (the user, 2026-10-24: one 3.4k-line file had grown
   to hold everything). EditorApplication keeps the lifecycle alone (configure, start the pieces,
   hand out the frame, shut down); each piece is a file and a partition of its own with its
   tests, ported in its own commit:
   - EditorWindow: GuiHost, the fonts, EditorResources and the editor sheet, screenshots after the
     GUI draws, toasts and the status line;
   - EditorSession: opening and closing a project, the manager mode, the settings stores, saving
     the layout;
   - EditorPages: open, close (the dirty prompt), save and save as, titles, the active page, the
     Game tab, cooking what a page needs, pending asset edits;
   - EditorNativeModule: a project's native code staged and reloaded;
   - EditorExport: export jobs, presets, templates, the resource memory report;
   - EditorAssetCreation: File > New's create-and-open;
   - the actions, one registration per menu (File, Edit, Project, View, Help);
   - the MCP host's start and stop with its tools.
   Tools.Editor comes back with a trimmed Main (no pages).
4. The frame and the dock: Shell on DockController and DockArea, OS floats through
   DockHostWindows, the Project, Assets and Console panels (the last two placeholders until X4).
5. Layout persistence: the store section holds a DockLayoutDocument (section version 2); a version
   1 tree migrates where it maps, else the default layout.
6. Pages through the dock: open, close with the dirty prompt, the active page, titles, reveal, the
   open pages saved.
7. The menu bar from the actions, File > New from the creators.
8. Shortcuts: the actions' chords into the keymap (Bind, Rebind with the user's overrides).
9. The status bar and toasts.
10. The command palette.
11. The small dialogs and the exit gates on MessageBox with button roles: Save As (a name), the
    unsaved pages at exit and at a project's close.
12. Preferences: theme, accent, density, motion and UI scale (EditorUiSettings), shortcuts, MCP.
13. The project home and the project manager.
14. MCP layout tools on DockLayout (layout_get's shape as the guide says; panel_dock as Edit).
15. MCP action tools with the GUI's modal interceptor.

Left for later phases: the Console's and Assets' contents (X4), the page toolbar and tool panels
(X5), the profiler panel (X7).

### 5.2 X4, the shared parts, in layers

Mapped from Editor.App on 2026-10-24 (about 10k lines across the console, the assets browser, the
asset slots and the dialogs). As in X3, each layer is one commit with its tests; GUI library
additions go first and are grouped with the other GUI commits.

1. The Console: the log on a virtualised GUI list, each line its time since the editor started, its
   level's colour, `[Category] message`; a chip per level as the old Console had (Debug, holding the
   trace lines too, Info, Warnings, Errors; the user's call over the mock's three, 2026-10-25) with
   their counts, a search, Clear; rows selected and copied; the newest line
   followed until the user scrolls up, and followed again at the bottom; an empty state. Fed each
   frame from the log buffer, which stamps each entry's time.
2. Asset drag data and the type-filtered drop: what an assets row carries, what a slot takes.
3. The asset slot (name, picture, pick, reveal, edit, clear, a drop target), the compact slot.
4. The inspector's asset reference on the GUI's property grid: GUI.Toolkit's Reference kind (a
   reflected type with Core's reference capability, its Guid as text, naming what it points at),
   and the editor's slot registered for it. It takes the source asset types whose builder's product
   is its runtime type or derives from it (ruling 19): no list in the editor. Entity references
   and the editor's own groups go with the scene page's inspector (X5): an EntityRef is no
   reference to Core, and only a scene names its entities.
5. The pickers and the small dialogs, one picker for assets, paths and groups as the kit draws it
   (catalog 2.4): a search over rows under their sections, favourites first; the current value
   checked and lit; Up and Down past the headings from the search, Enter picks, Esc keeps the
   current value; None when the owner allows nothing; with nothing to pick, what is missing and
   the owner's next step; with nothing matching, a way back. It opens as a popup below what it
   picks for. In three commits: the picker with the path source (files under a folder by
   extension, sorted, under their folders); the asset source (the accepted types, by group, the
   favourites; what the slot's pick opens); the group picker and Create Asset (the group tree, a
   name checked live beside the field, Create refusing with why). The confirm dialog is the GUI's
   MessageBox (verbs, roles, Escape its cancel), so ConfirmDialog goes.
6. Project Settings on the property grid (ruling 20), then Export Templates and Export, whose
   preset settings are the grid the same way (extra files a list, an override's rows shown while it
   overrides; a preset removed after asking).
7. The assets browser as the mock draws it: a toolbar (the breadcrumb, a search, a filter by type,
   tiles or a list) over the group tree and the content. In five commits:
   1. Browsing: the group tree (folder icons, Content at the top), the breadcrumb, subgroups then
      assets, each by its type's glyph, name, kind, cook badge and favourite star; tiles or a
      list, kept per project; a search over every group, cleared by its x; the type filter; empty
      and no-match states; Enter or a double click opens an asset or enters a group; Reveal; a
      drag source for the slots (several assets at once). The Assets panel shows it.
   2. The verbs: the row, background and tree menus (multi-selection throughout); New from the
      creators, rename in place (F2), duplicate, delete (asking, naming what goes), cook and
      rebuild, the export flag, favourites, copy guid and path. One AssetOperations does each, for
      the browser and for an agent's delete over MCP.
   3. Moving: assets dropped on a group (in the tree, a group's row, the breadcrumb) move there,
      the content database moving their files (a move it did not have).
   4. Pictures: thumbnails in tiles and rows as they arrive.
   5. Import: files dropped on the window (into the group shown) or Import... (into its group),
      through one import window for one file or many where the old editor had two (the catalog's
      inconsistency): where they go, the files with their states, the chosen file's importer,
      options and plan, Import waiting while it cannot, saying why. AssetImport does the work (the
      importers, the worker reads, the commit with its deferred writes) and the window shows it;
      a file needing no review imports at once.

### 5.3 X5, the scene page, in layers

Mapped from the parked libraries on 2026-10-25. Editor.Scene is about 20k lines (9.3k on
foundation.ui: the page, the hierarchy, the inspector, the scene actions' keys, the two picker
dialogs; the rest UI-free: the edit context and its undo commands, the gizmos and the select tool,
the component gizmos, the grid, the view gizmo, the zoom readout, the view settings, the camera
preview's sizing, the capture recorder, entity JSON, loading, settings profiles, the thumbnail
generators and the scene MCP tools), with 6.8k lines of headless tests. Editor.Play is about 4k
(2.2k on foundation.ui: the Game tab and its registration), Editor.Preview 0.9k. The design is the
mock's scene page (Data/Editor/Mock/scene.gml): the page's tool bar over its own hierarchy,
viewport and inspector (ruling 11), the scene the hierarchy's top row (ruling 12), the viewport's
HUD in fixed zones. As before, each layer is one commit with its tests; GUI library additions go
first and are grouped with the other GUI commits. The catalog's problems for these pieces
(editor-gui-catalog/scene.md, 1 to 13) are each answered where its piece is built.

GUI library additions X5 needs (GUI.Viewport, checked against the GUI first):
- the colour target's state readable and settable, so a renderer that owns its transitions (the
  render graph's TargetState) can draw into the viewport's target: done, ColorState and
  SetColorState, and with no OnRender a target drawn outside is kept, not cleared;
- an sRGB view of the colour target, for the game's screen UI blending in encoded space (with
  layer 7);
- no recording while the viewport is not shown: there already, GuiHost records only content whose
  node is shown, and a page asks its viewport's IsTreeVisible before rendering into it.
- for the hierarchy: a model's row classes the tree puts on the row (ModelRole::RowClass: a
  prefab's member, an inactive entity), and several items selected from code (an item view's
  SetSelectedIds: the viewport's marquee in the tree).

The layers:
1. Editor.Scene's UI-free half back in the build, with its headless tests: the files above, the
   scene actions' keys as the keymap's keystrokes, the UI files out of the source list, unedited
   (and the tests that drive them: the page's, the settings rows', the collision matrix's, back
   with their layers). Then the scene editor's services, none of them UI, out of the page's
   registration: the document types, the export's scene seams (the text transcoded to the wire,
   the references scanned for the closure) and a model import's prefab and scene, registered by
   Tools.Editor (they had been out of the build with the page since X2).
2. The scene page and its viewport, the page broken up as the application was in X3 (one class of
   3.3k lines held all of it): the page itself (its scene, its parts, save, an external change),
   its viewport (rendering through the renderer, the camera's input, picking, the capture and the
   project's picture), its overlays (the grid, the markers and selection boxes, the gizmos' draws,
   the zoom readout, the view gizmo, the frame rate), its view state (each scene's prefs, saved and
   restored), Simulate's run and snapshot, the prefab flows (create from a selection, apply, revert,
   spawn), and the factories and registration; each a partition of its own, the later layers' parts
   (the bar, the HUD, the tool panels, the camera preview) the same. SceneEditorPage and
   PrefabEditorPage on the GUI from markup,
   the viewport a GUI.Viewport rendering through the renderer with the editor camera, picking, the
   grid, the gizmos and the overlays; the view settings kept per scene; Save, Undo and Redo on the
   page's bar; a failed load shown on the page, not only logged; a prefab page marked as one. The
   scene MCP live tools contributed again (selection, simulate, entity_inspect, component_set, the
   viewport camera and screenshot, the entity edit tools). Editor.Preview's thumbnail stage back
   with the scene thumbnail generators (the assets browser's pictures of meshes, materials,
   prefabs, scenes and effects: the generators only queue, the stage renders), and the export's
   scene transcoding.
3. The hierarchy: a TreeView over the scene, the scene its top row, each entity by its type's icon,
   prefab members and inactive entities marked; multi-selection shared with the viewport; rename
   in place (F2); reparent and reorder by drag; the row and background menus through the actions
   only (one path for create and delete, refused visibly while simulating); a search with its clear
   and a guid; an entity's drag data for entity slots; assets dropped in (a prefab spawned, under a
   row when dropped on it).
4. The inspector, in four parts: (1) its head, Active, Transform and each component on the
   property grid (the editor's PropertySections: a source of sections; ruling of 2026-10-25, a
   piece only the editor uses lives in the editor), a component's verbs on its header (the grid's
   row-menu hook), Add Component, every edit one undo step, read only while simulating; (2) entity
   references (ruling of 2026-10-25: the reflected source takes a struct the application registers
   as a reference by its Guid property, as it takes curves; EntityRef reflected by its id); (3) the
   bespoke sections, by a registry of a component type's extras (notices, actions, sections of
   their own) the inspector asks, so it names no component: the rigid body's notice, the zone's
   Bake (at once on the main thread: a bake as a job waits for the navigation bake's one call to be
   split into a collect here and a bake on a worker), the script component's behaviours (a source
   of their own); (4) the scene's settings while its row is selected (ruling 12): a section per
   settings block, each property where the edit context says it lands (a profile's values while
   the block uses one, else the block's own), a block's profile verbs (open, make, copy into the
   scene), and the rows reflection does not show as a settings type's extras through the same
   registry: the physics block's collision groups (a list, each its name and the groups it
   collides with, a pair both ways; only the last removed, as a body keeps its group by index)
   and the scene script block's Level properties (a behaviour's rows, shared). In full: its head
   (icon, name, kind), a search over the properties, the selection's
   components on the property grid with the editors by hint (X4); a component's verbs (copy,
   remove, reset, revert to prefab); Add Component on the picker (by category, with icons and each
   one's description); an entity reference through the entity picker (the picker's tree request);
   the bespoke sections (a rigid body's shape notice kept current, a zone's bake as a job, a
   script's behaviours); the scene's settings when the scene is selected (profiles, the collision
   matrix); read-only while simulating, and shown so.
5. The bar and the HUD, in three parts: (1) the bar's editing controls, each an action with its
   shortcut in its tooltip: Select (Q, new: picks with no handles), Move, Rotate and Scale as one
   choice; the space (its icon the space it is in); Snap (every drag snaps while it is on, Ctrl
   for one drag as before) with its increments (move, rotate, scale) in its menu, where they had
   no UI; Vertex Snap; Frame. Disabled while the camera owns the input. The grid is an overlay, so
   it is the overlays chip's (ruling 22). (2) The HUD in its zones over the viewport, on the GUI
   rather than the debug font: at the top start the chips (projection: perspective or
   orthographic and the axis views; shading: Lit or Unlit (ruling 21), the debug terms in a
   submenu, the graph's textures through the picker (by pass, searched), the effects as what is on rather than "No X"; overlays: the grid and its
   plane, markers, the LOD tint, colliders, stats), each saying what is on; at the top end the
   stats and the view gizmo; at the bottom start the active tool's status and the zoom readout.
   (3) The tools on the bar from the tool registry (a category with several tools one menu, with
   their icons; an unavailable one disabled, its reason its tooltip), the active tool's panel
   (ruling 23), the panel host and its widgets on the GUI, and Terrain's and Vegetation's brushes
   and panels back with it.
6. The camera preview in the HUD's bottom end: titled with its camera, at the camera's aspect,
   pinned or following the selection.
7. Simulate and play in editor: Simulate on the page's bar (named so: Play is the Game tab's), the
   edits locked and shown so; Editor.Play's UI-free half back, the Game tab on GUI.Viewport (its
   resolution and phone frame, the keyboard captured with Shift+F1 to leave, X1), several
   instances, the PIE MCP tools. In four parts: (1) Simulate, Pause and Stop on the scene
   page's bar between its editing controls and its tools, as the mock draws them, each an action
   (the start one labelled Simulate), the state named beside them while it runs (Simulating,
   Paused: the kit's transport state), the edits locked meanwhile (the inspector read only and
   saying so, the hierarchy's verbs refused visibly, as before); (2) Editor.Play's UI-free half
   back in the build (the resolutions, the PIE run and instance seams, the game-audio settings);
   (3) the Game tab on GUI.Viewport with the kit's transport (Restart, Play, Pause, Stop and the
   state), its resolution and phone frame, the keyboard captured with Shift+F1 to leave, a cook
   it waits for named on its bar; (4) several instances side by side, the Game menu's Play and
   Play New Instance, and the PIE MCP tools. Part 4 needed nothing of its own: the Game menu and
   the layout tools open and place the tabs, and the PIE tools act through IPieInstancePage, so
   they worked on the ported tab as they were (played on Sky Hopper, 2026-10-25: two instances
   side by side, each scripted on its own, a capture, the phone, Stop).

The property animation panel under the viewport comes back with Editor.PropertyAnimation (X6).

### 5.4 X6, the asset pages, in layers

Mapped from the parked libraries on 2026-10-26 (the catalog's pages.md surveyed them on
2026-10-23). Twenty libraries, 200 to 4,200 lines each, every page a UIEditorPage on foundation.ui;
what is not UI in them (the thumbnail generators, the preview settings types, the lexers' language
tables, the stat lines, the form scan) carries over unedited. Every widget they need is in the GUI
already: the property grid and its reflected source (with curves, gradients and references),
CurveView, GradientView, Dopesheet and TimeAxis, GraphView, GUI.Code's CodeEditor with its
languages, completion and find bar, GUI.Viewport, the Splitter, TreeView and ListView. So X6 adds
no GUI library work of its own; a gap found on the way is a GUI commit grouped with the others.

What every page shares, from the kit (Mock/kit.gml): one page bar, Save, Undo and Redo first,
bound to the actions over the page (PageActionBar), the page's own after; one transport for every
preview (Restart, Play, Pause, Stop, the state named); a page that cannot read its asset says so on
the page (the kit's failed state), not only in the log; read-only facts as read-only rows, never as
fields with no setter (the catalog's inconsistency 8); a preview's own choices (its mesh, its shape,
its scene) in the preview's bar, never among the asset's rows (inconsistency 10); every edit one
undo step on the page's stack. The catalog's other problems are each answered where their page is
built. As before, each layer is one commit with its tests, and the editor is run on a project and
screenshotted over MCP before it is reported.

The layers:
1. The shared parts and the fallback page: the XML text format a reflected source writes its undo
   in moves from the scene's inspector to Editor.App, beside an undo command for a reflected
   source's change over any object (what the scene's settings do, made common); Editor.Generic on
   the GUI, its serialize-scanned fields a property bag on the grid (an array's fields a group,
   folded, as before; a blob's size a read-only row; a guid one asset reference, not a slot and a
   text row), from its markup.
2. The image pages: Texture, Image, Heightfield and Fonts, each the picture (an image with zoom
   and pan, its checkerboard under alpha) beside the grid; their facts read-only rows; the
   texture's presets on its bar.
3. The preview viewport: Editor.Preview's PreviewViewport on GUI.Viewport (a private scene, the
   editor camera, a frame button and the camera's controls as a hint, one backdrop for every
   preview); then the mesh viewer (Geometry, its stats a read-only grid) and the collision shape
   page (Physics: its edits on the undo stack, which it never had).
4. The material page (Materials) and the settings profile page (Render), each a preview beside
   the grid, the preview's shape, mesh or scene on the preview's bar.
5. The audio pages and the input map: Audio's clip (the waveform a widget of the editor's, its
   transport the kit's), its sound cue (its slots a list on the grid) and its bus layout (the
   GUI's tree beside the grid); Input's map (its sets, actions and bindings, the key capture the
   kit's).
   The input map's design (user 2026-10-26, from three mocked in the GUISandbox's editor mock,
   Data/Editor/Mock/input-*.gml): the sets and their actions a tree beside the grid; the tree's bar
   adds sets only and removes the chosen row (an add there would guess which set); a set chosen
   shows its name and priority and its actions as the grid's list, its Add adding to that set, each
   action a group opening in place (its kind and interaction, its bindings a list of their own, its
   processing), closed showing its name, kind and bindings in a line; an action chosen in the tree
   opens it there. In parts:
   1. The grid: each level's names start past its parent's (it did already: a list element's grip
      sits in its indent), and a closed group may say what it holds in a line beside its name.
   2. The reflected source: the application describes each row as it is listed (a hook over the
      row's path), so the page names an action by its name and sums it up in a line, and shows a
      binding's settings by its source, without the input types knowing the editor.
   3. The page (Editor.Input): the tree, the set's rows from the reflected source over the chosen
      set (each edit one undo step), a binding's input captured by pressing it (an editor of the
      editor's, listening to the devices the action's kind takes).
6. The code pages: the languages (AngelScript, Luau, HLSL) as GUI.Code languages, the markup and
   the sheet languages for the GUI's own files (XML and CSS highlighting); Script (completion, the
   API browser as a tree beside the code, the problems list), Shaders (the code beside a preview
   and the shader's grid) and the GUI's documents and sheets (the code beside a live preview), each
   with the kit's problems list rather than a status line.
   In parts (surveyed 2026-10-26; the GUISandbox's Code page proved the GUI.Code side):
   1. The languages: one definition each, a data file read through the XML serialization, shared
      by the editor and the sandbox in Data/Assets/code (user 2026-10-26: one folder, not a copy
      each); the script modules give the editor AngelScript and Luau, Editor.Shaders HLSL,
      Editor.GUI XML and CSS (CSS coloured for the first time).
   2. The problems list, a part of the editor's kit (Editor.App, until something else needs it):
      the errors and warnings a check found, each with its severity's icon, its place and its
      message, a click going to its line; it replaces the old pages' inert error boxes and the GUI
      page's one-line status.
   3. The Script page: the code (its language by the asset's, checked as it is typed) beside the
      API browser (a tree with its search, in a splitter, opened from the page's bar rather than a
      button in a status row), completion and hover from the API the runtime binds, breakpoints in
      the gutter and the execution line shared with the Game tabs, the problems list under the code.
   4. The Shader page: the code and its problems beside the preview and the shader's grid, in
      splitters; the name and entry points edited on the undo stack; a preview showing the last
      shader that compiled says so.
   5. The GUI document and sheet page: the code and its problems beside the live preview, in a
      splitter; a sheet's preview markup the page's own, not an edit of the asset.
7. The terrain page (Editor.Terrain's), its layers a list on the grid, beside the preview; and a
   vegetation mask page, new (user 2026-10-26: no editor had one, a mask opened on the generic page):
   its density planes as pictures on the image view, its options on the grid, as the heightfield
   page's; its painting stays the scene view's tool.
   In parts:
   1. The terrain page: the preview pages' shape, the cooked terrain under a sun beside the grid. The
      asset keeps its layers in parallel arrays, so the rows are the page's own: the terrain's
      (heightfield, splat weights, shadows, height blend, layer texture size), the base layer's
      (its four maps and tiling), and the paint layers a list of groups (five maps and tiling each),
      named by channel ("Layer 1", the albedo's name while closed: the property bag's describe hook,
      as the reflected source has). A layer's place is its channel in the weights: the list adds and
      removes, never reorders, and a removal frees its weights as before. New splat weights are the
      bar's (a size from its menu), made beside the terrain; the cooked terrain's grid, world size
      and chunks are read-only rows. Each edit one undo step.
   2. The vegetation mask page: the image pages' shape, a density plane at a time (none to full
      as black to white, chosen in the channel's place, the pointer's density read out) beside the
      mask's options. As its cook reads them (user 2026-10-27): a painted mask's width and height
      are read only once anything is painted (a notice says why), its plane count edits (the cook
      keeps the planes that remain, a new one empty); an imported mask's planes are its image's
      channels and its options read only. Its painting stays the scene view's tool.
8. The particle effect page (Particles): the systems a tree, the preview, the inspector with its
   curves and gradients on the grid's own editors (a gradient's stop recoloured, which nothing
   handled before).
   In parts (surveyed 2026-10-27: the old page built every row by hand, a chain of casts naming
   each module type, while the modules were already reflected):
   1. The reflection: every authored field of a system reflected (its texture, mesh and materials
      as references to what they bind, its particle budget through its setter, its trail and
      flipbook), each row shown only where it does something (a render mode's sections, an emission
      mode's spawn fields, a shape's own measures, by `visibleWhen`), the measures that have one
      ranged; the collision lists without their counts beside them; the sub-emitter links. A
      system removed takes its links' places with it (one into it does nothing, those after it
      move down), and an effect read over another replaces it (an undo of the whole asset doubled
      its systems). Then each system owns its sub-emitters (user 2026-10-27; a divergence from
      Sedulous, whose links are the effect's): a link was the effect's and named only the system
      it spawns into, every other system's births or deaths setting it off, so no page could say
      whose it was and a third system's deaths spawned into it too. A link is now a system's, from
      its own births or deaths into another system; the authoring asset's data version 1 reads
      version 0's effect links onto every system but the one each spawns into, as they behaved. A list of bare ids names what its elements point at by
      the list's `referenceType`, and four floats kept as a colour are one (GUI.Toolkit).
   2. The page (Editor.Particles): the preview pages' shape with a third pane, the systems a tree
      (each its emitter, initializers and behaviors; the tree's bar adds a system and removes the
      chosen system or module), the preview with the kit's transport, a speed and the live count on
      its bar (the chosen system's emission shape drawn over it), and the grid the chosen system
      from reflection: its modules lists of the grid's own, added by type from what derives from
      the list's base (no list of module types in the page), each opening in place, a module
      chosen in the tree opening there; curves and gradients the toolkit's. A system's
      sub-emitters (none of the old page's) are a list of its rows and a folder of its tree, each
      into another system chosen by name and named by what sets it off ("On death into Sparks").
      A row the user folds in the tree stays folded. Every edit one undo step. The preview's transport plays, pauses and clears the preview and never writes the
      effect (the old Stop and Play set every system emitting, a sub-emitter's child too).
9. The property animation panel under the scene page's viewport (PropertyAnimation): its clips'
   tracks on the Dopesheet, a track's curve on CurveView, its transport the kit's. A clip is an
   asset of its own, not the scene: the panel has its own history, its own Undo, Redo and Save on
   its bar and its own unsaved mark, and while it is open and holds the focus the keys go to it
   (Ctrl+Z, Ctrl+Y, Ctrl+S and the rest act on the clip, not the scene; user 2026-10-27: the old
   panel's edits on the scene page's stack read as the scene's); a key it does not handle goes on
   to the scene page and the editor (the GUI's commands bubble along the focus path: the panel's
   root handles the actions' commands it binds, and lets the rest propagate). A key it binds
   stays with it while it holds the focus, so Ctrl+Z on a clip with nothing to undo never
   reaches the scene. Its preview poses the scene's
   entities without touching the scene's history or its unsaved mark; an unsaved clip is asked
   about on its own when the scene page closes or another clip opens.
   In parts (surveyed 2026-10-27: the toolkit's time axis, ruler, dopesheet and curve view take
   sources; the old panel's binding, preview, track seeds and transport carry over):
   1. The UI-free half (Editor.PropertyAnimation): the clip document (a clip asset loaded, saved
      and cooked, its unsaved mark, its own history: each edit the whole clip before and after);
      the clip as the dopesheet's and the curve view's source (a channel per scalar curve,
      "Light.color.r", a rotation track one channel of its keys; a key inserted takes the clip's
      value there; the curve view the chosen tracks' curves); the track seeds (a bound entity's
      animatable properties); the preview (the bound entity posed at the playhead, every value it
      wrote put back when it stops, never a step of the scene's); the transport's clock.
   2. The panel on the GUI from its markup: its bar (the clip's name and unsaved mark, New, Open,
      Save, Undo, Redo, the bound entity and Use Selected, Add Track, the kit's transport and
      Loop, the clip's length), the ruler, dopesheet and curve view on one axis, a state with no
      clip; the keys it binds handled on its root (the actions' commands), the rest let through.
   3. On the scene page: under the viewport in a splitter (its pane out of the splitter while
      hidden, so the viewport has it all), shown from the Scene menu's Animation Panel and from an
      animator's Edit Clip in the inspector, its preview stood down while simulating and put back
      before a run starts and before the scene saves (what it posed is never the scene's); an
      unsaved clip asked about apart from the scene when another clip opens, and beside the
      scene's own changes when the page closes or the editor exits (a page's unsaved work besides
      its asset, Editor.Core and Editor.App). New names and places the clip in Create Asset
      (Animations and the entity's name to start), opens it, and gives it to the entity's
      animator when that has no clip yet, one step of the scene's (user 2026-10-27); an animator
      playing another keeps it. The bar ends with the clip's name and unsaved mark, after its
      space, so no button moves as the mark comes and goes; the curve view fits its values to what
      it shows whenever that changes (never its time, which the dopesheet shares), and the bar's
      Frame frames the curves in time and value.
10. The animation pages (Animation): the clip (a preview, its events as markers on the time axis),
    the skeleton (the bone tree with a search beside the preview) and the graph (GraphView over the
    preview, its layers and parameters as lists).
    In parts (surveyed 2026-10-27: the old pages built every row by hand, over parallel arrays and
    kinds kept as numbers, nothing reflected; the graph view already takes node links, a state
    machine's transitions, and draws them as straight arrows; the time ruler has no markers):
    1. The ruler's markers (GUI.Toolkit): a time ruler shows a source's markers, each a time and a
       label, as a flag at its time; a click chooses one, a drag moves it, one gesture through the
       source (Escape cancels it), and a marker under the pointer names itself in full. The undo is
       the application's, as every source's.
    2. The clip page: the preview pages' shape with the ruler under the preview, on the clip's time:
       the playhead the kit's transport's, the clip's events its markers (one dragged moves its
       time, one step). The preview's own choices on its bar (the skeleton and skinned mesh it plays
       on, kept per asset, and Travel, which had been among the clip's rows). The rows are the
       page's own: the clip's facts read-only (its name, duration, tracks and keys), Looping, the
       root motion's bone and axes, and its events a list, each a time and a name, a new one at the
       playhead. Each edit one undo step. The skeleton drawn as bones and the path a clip travels
       are Editor.Animation's, shared by its pages.
    3. The skeleton page: the bones a tree with its search beside the bind pose, the chosen bone
       drawn apart from the rest; the skeleton's facts (its bones, roots and depth) and the chosen
       bone's (its index, parent and bind position, rotation and scale) read-only rows. A viewer:
       nothing it shows edits.
    4. The graph as a graph source (UI-free): a layer's states its nodes, and Any State, which only
       begins a transition; its transitions node links; where each sits kept on the asset as
       before. A state deleted takes its transitions with it and moves the indices after it down;
       a layer's or a parameter's removal remaps what named it (a condition or a blend tree's
       driver on a removed parameter cleared). Every change one undo step of the whole asset.
    5. The graph page: the layers and the parameters as lists beside the graph view over the chosen
       layer, the preview under it with the kit's transport, the state it plays lit on the graph;
       the grid the chosen layer, parameter, state or transition (a transition's conditions a list,
       a blend tree's entries a list, each a clip and its threshold or place); a new graph from
       Create Asset, as before.

### 5.5 X7, the profiler, in layers

Built 2026-10-27, layers 0 to 5 (the look the user's to judge). Running the page found the views'
sheet parts unstyled (a part the sheet styles must be named by the widget's CollectStyleParts),
fixed in Profiler.GUI; the engine registers a dozen counters, so their plots stand three high and
scroll. The Profiler is closed in the default layout and opens in the page area from View >
Profiler (or Profiler Record); a layout saved before it shows it once.

Surveyed 2026-10-27, for the user's choices (a tab in the page area; a new library; survey and
mock first). Today's panel (Editor.App's ProfilerPanel on Profiler.UI, Specs/profiler.md P5) is a
tab docked with the Console: Record, Stop, Save, Open and Live; a frame strip (a click one frame, a
drag a range); the timeline (a lane per thread, the GPU lane; the wheel zooms about the pointer, a
drag pans, a hover names a bar); beside it the scope table (total, self, calls; sorted by a
heading) over the counters; a name chosen in the table lit on the timeline. Profiler.UI's
ProfileWindow (the frames chosen, their stats and totals) is UI-free already; its views are
foundation.ui, and so is the in-game overlay Engine.DefaultApp hosts.

What the references do, per concern (Godot, LumixEngine, traktor, ZeroEngine, LunarSong and ez
from their source; Tracy from its manual; Unity, Unreal Insights and Superluminal from their
documentation):
- **Where it lives.** A bottom or debugger tab (Godot, Zero, LunarSong), a window of its own
  (Lumix, traktor, every dedicated tool), or nothing in the editor (ez hands off to Tracy). Godot
  and LunarSong split it into several panels (script, visual, monitors); the dedicated tools keep
  one window with the frame strip on top driving everything under it.
- **The frame strip.** Everywhere a ring of frames (Godot 60 to 10,000, Unity 300 to 2,000, Zero
  512, LunarSong 600; ours 3,600). A click picks a frame, a drag or Shift and a click a range
  (Tracy, Unreal, Unity's Profile Analyzer). Bars coloured against a budget with its lines (Tracy;
  Godot and LunarSong a chosen target fps); the frames in the timeline's view boxed on the strip
  (Tracy). Following the newest stops as soon as one is chosen (Tracy, Unity, LunarSong). Profile
  Analyzer selects the longest or the median frame from a menu. Avoid: a fixed scale that clips a
  spike (Zero), axes with no values (a Unity complaint), no zoom on a long strip (Godot).
- **The timeline.** Threads as row groups with depth inside (Tracy, Lumix, traktor, Unity,
  Unreal), collapsible, hideable; GPU passes drawn exactly as CPU scopes in their own rows
  (Tracy); counters as plots under them with their min and max printed (Tracy, Lumix); the ruler
  and the frames' row pinned on top. The wheel zooms about the pointer and a drag pans (all;
  Lumix needs Ctrl, traktor Alt). One gesture zooms to a scope, a frame or a lane (Tracy's middle
  click, Unity's F); one fits the frame (Unity's A, Lumix's reset). A scope too small to draw is
  kept as a marked block with its count (Tracy), never dropped: names vanishing on zoom is a Unity
  complaint. A name drawn only where it fits (Lumix). Colour by the scope's name, so a scope keeps
  its colour across frames (Godot's hash; per-thread colour, traktor's, makes scopes hard to tell
  apart).
- **Selecting a scope.** A hover outlines every scope of that name (Tracy); a choice lights its
  instances and dims the rest (Unreal; Lumix's filter, filled by a double-click), and shows its
  share in the frame strip (Unity; Tracy plots its time per frame). Hovering a CPU scope lights the
  GPU pass it recorded (Lumix's links).
- **A range.** A drag across the timeline marks a span, shaded, its length read out; the tables
  recompute for it (Tracy, Superluminal, Unreal).
- **The table.** Name, count, inclusive and self (all), mean and max per call (Tracy, Unreal),
  share of the frame (traktor, Godot); every heading sorts; a name filter (Tracy, Lumix, Superluminal).
  LunarSong's default order is the call tree and any other sort flattens it to a hot list. Unreal's
  "find instance" jumps to the longest one.
- **One scope's detail.** Tracy's zone info: its parents, its children grouped by name, the
  spread of its durations (a histogram, Tracy and Superluminal).
- **Recording.** Record and stop (Godot, traktor), always on with pause (LunarSong, Tracy), or an
  always-on ring snapped on demand and on a spike: Lumix stops itself when a frame passes N ms,
  the most useful for catching a hitch.
- **Comparing two captures.** Only the dedicated tools (Tracy's Compare, Profile Analyzer): two
  sets' histograms overlaid, the difference coloured by size. None of the engines.
- **Density.** Tracy's depth is praised and its gestures (each button, with Ctrl or Shift) and
  floating windows are not; Unreal offers a compact mode and a depth limit per lane.

The design (proposed; the mock is judged before anything is built):
- **The page.** One Profiler tab in the page area, opened from View, Profiler as today, floated
  like any page; its layout id stays `profiler`. Top to bottom: the page bar (Record and Stop,
  Live, Open, Save; the source named; the budget, a choice of 30, 60, 120 fps or none; Stop on
  spike over a time, Lumix's, off by default), the frame strip across the full width, the timeline
  across the full width taking what is left, and under it a split: the scope table, with the
  chosen scope's detail beside it. The full width is what the page area gives over today's panel:
  the timeline no longer shares its width with the table.
- **The frame strip.** Bars coloured against the budget with its line and a scale with values;
  the timeline's view boxed; a click a frame, a drag a range; Longest and Median in its bar; the
  chosen scope's time per frame drawn in each bar when a scope is chosen. Following the newest
  stops at a choice and comes back with Live. The counters under it as plots over the same
  frames, each with its range printed: a counter is set once a frame, so over the frames is where
  it says something (Unity's charts), where a plot along one frame's timeline would be flat.
- **The timeline.** The ruler and the frames' numbers pinned; a group per thread (its name, a
  fold), depth inside; the GPU group. Wheel zooms,
  drag pans, a double-click zooms to what is under the pointer (a scope, a frame on the ruler, a
  lane's name), Fit in its corner. A drag on the ruler marks a range. Colours by name; a scope too
  small to draw a hatched block with its count.
- **Selection.** A hover outlines the name's other instances and names the bar with its duration;
  a click chooses the name: its instances lit, the rest dimmed, its row chosen in the table, its
  detail shown, its time per frame on the strip. The table's choice is the same choice.
- **The table.** The tree (parents over their children) in its default order, a flat list sorted
  by any other heading; columns Name, Calls, Total, Self, Mean, Max, % of frame; a filter; GPU
  passes their own group in it. Over a marked range, the range's.
- **The detail.** The chosen scope's parents, its children by name with their share, and its
  durations' spread over every frame held (one frame chosen has too few calls to spread), the
  bars holding the chosen frames' calls lit; with Longest (the longest instance zoomed to).
- **Not in X7:** comparing two captures, and the overlay's move (X8 deletes Profiler.UI with the
  old UI once the overlay is on the GUI).
- **Keys.** The docking ruling (mouse first; ask before adding default shortcuts) holds: every
  gesture above is the mouse's, and the keys the references bind (fit, zoom to the chosen scope,
  W A S D) wait for the user.

The layers:
0. The mock: `Data/Editor/Mock/profiler.gml` in the GUISandbox's editor mock, a static frame of
   made-up data (the strip and two counters under it, a frame's timeline with its threads, a
   folded group and the GPU, a range marked and a scope chosen, the table and the detail), dark
   and light, for the user to judge.
1. ProfileWindow moves down to Profiler (UI-free, read by both libraries while Profiler.UI lives).
2. Profiler.GUI (`foundation.profiler.gui`), the frame strip: the model and the view, with tests.
3. The timeline: groups, folds, zoom and pan, zoom to, the range, hatched blocks.
4. The table and the detail, the shared choice.
5. The editor's Profiler page on them, from its markup: Record, Live, Open, Save, the budget,
   stop on spike; the MCP profiler and layout tools as before; the old panel's files deleted.
   McpGuide in step where a tool's text changes.

### 5.6 X8, the parity check and the swap

Checked on 2026-10-27, the old editor read at 4fe177db2 against the new one (the files in a
source list only), surface by surface: the frame, the scene page, the asset pages and the fields.
The user's ruling for the swap: the old editor goes, no other library does yet (foundation.ui,
Engine.UI, UI.Toolkit and Profiler.UI stay; DefaultApp's overlay stays on Profiler.UI).

**What holds.** Every old action id (29) with its chord, every old MCP tool (the editor's 78 less
`project_create` and `project_open`, the stdio host's), every Tools.Editor flag, every menu item,
dialog and gate; every asset type's page (GameUI's apart, ruling 10) and every thumbnail
generator; all 25 old scene actions. The sample projects open and play in the Game tab from a
fresh copy: Sky Hopper, PaperKid, Snowline, Lamplight, Shader Gallery and Masquerade, no error
in the log (NativeSample needs the shared build for its module, as before). The playthroughs are
the user's.

**The gaps**, old behaviour the new editor lacks or does worse (old paths at 4fe177db2):

- The fields, shared by the inspector and the pages:
  1. A `boneName` string offers the skeleton's bones and "(not in the skeleton)"
     (Editor.Scene/InspectorViewImpl.cpp:2416); the IK and aim fields are plain text now.
  2. An asset slot shows the asset's thumbnail, the type's glyph only without one
     (Editor.App/ResourceRefEditorImpl.cpp:155).
  3. An asset or entity dropped on a list appends to it, a wrong type refused with the types it
     takes (Editor.App/ContainerListEditorImpl.cpp:26-76): materials, reflected lists, project
     fonts, particle materials, behaviours.
  4. A component with no reflection shows as a section under its serialization id
     (InspectorViewImpl.cpp:1063); it is skipped now.
  5. A `visibleWhen` with no name or no value is refused, as the old parser did.
- The frame:
  6. The Export finished toast stays, with Open Folder (Editor.App/ApplicationImpl.cpp:1544).
  7. The asset picker: Pin and Unpin on a row's menu; a typed guid of the wrong type says so;
     thumbnails in list mode too (AssetPickerDialogImpl.cpp:139, 156, 295).
  8. The assets browser: a slow second click renames; a whole guid typed selects its asset
     (AssetsView.cppm:330, AssetsViewImpl.cpp:733).
- The scene page:
  9. Paste Component on the inspector for any selected entity, not only on a header's menu
     (InspectorViewImpl.cpp:3213).
  10. The property animation panel: the previewed entity marked in the view
      (PropertyAnimationPanelImpl.cpp:1450), Bind through the entity picker (:269), a clip asset
      opened into the panel while a scene page shows (ScenePage.cppm:371), the panel's toggle on
      the bar (ScenePageImpl.cpp:1670).
  11. The terrain's Paint Splat lists the base layer (erase to reveal) above its layers
      (Editor.Terrain/ToolPanelsImpl.cpp:308).
- The pages:
  12. Fonts: the source file relinked with Browse (Editor.Fonts/FontEditorPageImpl.cpp:571).
  13. The Profiler: Clear (the frames dropped, the recording goes on); a counter's mean over the
      frames chosen (Profiler.UI CounterStrip).
  14. The graph page: Bones and Mesh toggles on the preview (the mesh hidden, its pick kept);
      Delete on a layer's or parameter's row menu (AnimationGraphPageImpl.cpp:201, 567, 571).
  15. The particle page's tree: a row menu (add a system or module, move up and down, delete),
      a system renamed in place.
  16. The input map: its totals line; each binding whose source the action's kind does not take
      marked on its row; "Listening" said while a capture waits.
  17. The script page names the class it compiled.
  18. The colour picker shows the colour it opened with beside the new one.
  19. Tools.Editor checks the thumbnail generators' counts at startup (Main.cpp:357).
- The old tests left out of the build (Editor.Scene.Tests' SceneEditorTests, CollisionMatrixTests,
  SettingsProfileTests, InspectorConditionTests; Editor.App.Tests' Shell, PageToolbar,
  ContainerListEditor, EditorIcons): 20. what they checked that holds in the new editor and has no
  test in the build gets one (the camera's dolly floor, unique names from the creators, Copy ID,
  the tree's keys moving the selection, a new scene's sun, a profile's edits and Copy Into Scene,
  Make Profile, the scene's bounds, the render components' attributes).

**For the user to rule**: the last collision group removed now sets its bit in every mask where
the old cleared it (deliberate in the new code, its test says the old); a material takes an
uncooked shader with a notice where the old refused it; the cue's variants are packed where the
old kept fixed slots; composite keys as rows, not a keyboard cross; curves and gradients edited in
a popup, not on the row; "grid as lines" dropped; what GameUI's pages did (the .sml and .sss
source, their live preview, tag completion on "<") is gone with them.

**Found on the way, not the editor's**: a particle effect names its texture by a string
(`textureRef`), so the scene page's first cook, of the scene's closure, leaves it out and the
effect binds nothing until Play's cook (Snowline, Lamplight).

The layers:
1. The swap: the old editor's files deleted (the unbuilt files left in Editor.App, Editor.Scene
   and their tests, and Editor.GameUI with its tests), the ledger and the build's comment in step.
2. The fields (gaps 1 to 5).
3. The frame (6 to 8).
4. The scene page (9 to 11).
5. The pages (12 to 19).
6. The old tests' behaviours (20).

## 6. Tests

Editor tests move from UIContext with synthetic events to GUI.Testing (Finder by selector or id,
Tester with a fixed clock). The shell's headless layout tests and the MCP layout tests keep their
assertions. A screenshot test is not proposed: looks stay the user's.

## 7. The mirror

The plan is written so the mirror ports it in the same order: X0's sheets, icons and mock are data
and copy as they are; X1 is GUI library work; X2 onwards follows the editor's libraries.

## 8. Not in scope

Removing foundation.ui, Engine.UI or Editor.GameUI; a theme editor; extension themes; platform
accessibility adapters; popups as OS windows (the current editor has none); the
press-drag-release menu bar.

## 9. Rulings and open questions

Rulings (the user, 2026-10-23):

1. A parallel editor on a branch; non-UI code moved over as it is needed. No page on the old UI
   inside the new frame: the two UIs never run in one editor.
2. Pages are markup (`.gml`) and a sheet, under `Data/Editor/`, with code binding by id.
3. The icons are all drawn by us.
4. Type icons: whatever suits the use (section 3.2: tinted by category in scanned lists,
   monochrome in bars and menus).
5. Fonts: Inter and JetBrains Mono.
6. Dark and light both ship.
7. A new accent, chosen in X0.
8. Menus keep opening on release. Menu keys become rebindable. Popups as OS windows are not
   needed (the current editor has none).

9. Rewritten in place on `editor-gui` (off vg at 4fe177db2); parked libraries leave the build,
   never commented out; vg gets no more changes.
10. Editor.GameUI stays with the old editor.
11. A page carries its own panes: a scene page its hierarchy and inspector, in its own splits, so
    pages stand side by side; the dock holds pages and the shared panels (assets, console, profiler).
12. The scene's own settings are the scene selected: the hierarchy's top row is the scene, and
    selecting it (or selecting nothing) shows the scene's settings in the same inspector. No
    Entity and Scene tabs.
13. The accent is azure (#2f6fdf); the default density is compact (0.85).
14. Menus, combo lists and tooltips close at once; only dialogs (and toasts) fade out (2026-10-24).
15. Vector fields in a narrow inspector stay as they are for now: each part hides its arrows below
    32 px of number room and then shrinks (2026-10-24; stacking or dropping the letters later).
16. MCP's layout_get gives a split as it is in the GUI's dock: `{split, children, weights}`, any
    number of children (2026-10-24); the guide changes with it, panel_dock's arguments do not.
17. Preferences' theme is Dark or Light for now; System (following the platform's preference, which
    the shell cannot report yet) comes later (2026-10-24).
18. Preferences has no font paths: the editor's fonts are its sheet's, and EditorFontSettings is gone
    (2026-10-24; no outside users, so no migration: a file that still has the section keeps it as
    an unknown one).
19. An asset reference (`Ref<T>`) takes the source asset types whose builder's product is `T` or
    derives from it: asked of the builder registry, never a list of types in the editor
    (2026-10-24).
20. Project Settings is the property grid over a bag the dialog builds from the settings'
    reflection (labels, categories, ranges, enums), staged with Save and Cancel. Its asset settings
    are bare Guids naming their source asset type (`assetType`), so they are Reference rows to that
    type, and a reference takes the source types whose builder makes what it points at, else the
    type itself when it names a source asset type (2026-10-25).
21. The scene view's shading is Lit or Unlit (the albedo term, named for what it shows); a
    wireframe waits for the renderer (WebGPU has no line fill, so it needs a pass of its own:
    backlog). The other debug terms stay under the shading's menu, a submenu; the graph's textures
    (dozens a frame, the passes' own: AO, bloom, SSGI, shadows...) are the picker's, grouped by
    their pass and searched, where they had been one long menu (2026-10-25).
22. The view's controls (projection, shading, overlays) are the HUD's chips over the view they
    change, each saying what is on; the page's bar keeps the editing controls (2026-10-25).
23. A tool's panel opens in the HUD's top start, under the chips, and moves as the old floating
    panel did: dragged by its head anywhere in the viewport (kept inside it as it shrinks), its
    place kept per scene, a double click on its head putting it back in its zone, its chevron
    folding it to its head. The editor's own piece, as the GUI has no floating panel
    (2026-10-25).
24. The asset picker is shaped as the assets browser: the project's groups a tree at its side
    (Favourites and All first), what the chosen group holds of the slot's types beside it as tiles
    or a list (the picker keeping the last one chosen), the current value checked, the search over
    every group while it has text (each match saying where it lives) (2026-10-25; the mock's
    asset-picker.gml).
25. The entity picker shows the scene as its hierarchy does: the hierarchy's tree model in a
    picker's frame, the scene its top row (not a pick), each entity by its type's icon in its
    category's colour, prefab members and inactive entities marked, levels opened and closed by
    their arrows, the current entity lit, the search keeping each match with what holds it
    (2026-10-25; the mock's entity-picker.gml).
