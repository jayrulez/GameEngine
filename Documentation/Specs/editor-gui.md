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
| Editor.App | In the build in parts: the UI-free files, the page's content a GUI node, the application broken up (window, session, native module, export, asset creation, layout store, MCP host, actions), the dock with the Project, Assets and Console panels (the last two a line each until X4), its layout saved with the project (an old dock's tree read into it); the rest out, unedited, until their layers | X3 (layers 1 to 5) |
| Tools.Editor | In the build with a trimmed Main: no pages yet | X3 (layer 3) |
| Editor.Scene, Editor.Play, Editor.Preview | Parked | |
| Editor.Animation, Editor.Particles, Editor.PropertyAnimation | Parked | |
| Editor.Generic, Texture, Fonts, Image, Heightfield, Render, Vegetation, Audio, Materials, Geometry, Physics, Input, Terrain | Parked | |
| Editor.Script, Editor.Script.AngelScript, Editor.Script.Luau, Editor.Shaders, Editor.GUI | Parked | |
| Editor.GameUI | Left with the old editor (ruling): never ported, deleted at the swap | |

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
| X7 | The profiler panel (foundation.profiler.ui onto the GUI). |
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
- a modal-open interceptor on the GUI context (MCP's unattended actions close the dialogs they
  open as cancelled; foundation.ui's DialogInterceptor).

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
11. The small dialogs and the exit gates on MessageBox with button roles.
12. Preferences: theme, accent, density, motion and UI scale (EditorUiSettings), shortcuts, MCP.
13. The project home and the project manager.
14. MCP layout tools on DockLayout (layout_get's shape as the guide says; panel_dock as Edit).
15. MCP action tools with the GUI's modal interceptor.

Left for later phases: the Console's and Assets' contents (X4), the page toolbar and tool panels
(X5), the profiler panel (X7).

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
