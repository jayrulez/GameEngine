# Editor shell UI catalog

Scope: Code/Editor/Editor.App (all), Editor.Core (actions, menus, shortcuts, settings shown in UI), Editor.Project (templates and icons shown in UI), Code/Tools/Tools.Editor/Main.cpp, and foundation.profiler.ui as used by the Profiler panel. Paths below are relative to Code unless absolute.

Each entry: Purpose (task and data, independent of layout), Files, Kind, Opened by, Regions and controls (top to bottom), Commands, States, Interactions, Drawing and icons, Problems (friction, missing feedback, awkward flows), Inconsistencies (versus other pieces).

Shared facts that apply everywhere:

- Theme: RoundedDarkTheme with the "Graphite & Orange" palette (ApplicationImpl.cpp ~272). A `property-field` class override shrinks property-grid fields to 12px with 5x2 padding. Many views still hard-code RGB literals for text colours (dim greys 0.5 to 0.72, gold, green, red) instead of theme roles.
- Dialogs are `foundation::ui::Dialog` (Foundation/UI/Overlay/Dialog.cppm): modal, centred, title label, content, right-aligned button row in the order buttons are added. Escape closes (Cancel). There is no default button and no Enter-to-confirm except where a dialog implements it itself (only the Command Palette does). Dialogs are not resizable by the user; each sets its own Min/Max.
- Every command surface is generated from `EditorActionRegistry` (Editor/Editor.Core/Actions.cppm): menu bar (menuPath), global shortcuts (effective chord), page toolbars (by id), command palette, Preferences > Shortcuts, MCP `action_*` tools. Actions are nullary over a "subject page" (the active page, or the toolbar's own page).
- Notices: `EditorContext::Notify(kind, text)` shows a toast AND overwrites the status bar text (ApplicationImpl.cpp ~355). `SetStatus` writes the status bar only.
- Undo commands carry no name, so Undo/Redo can never say what they undo (Editor.Core/Command.cppm).

---

## 1. Frames and windows

### 1.1 Main window frame (EditorShell)
- Purpose: host every editor surface for one open project: document tabs (one per open asset page), the global tool panels (Assets, Console, Profiler), the menu bar and the status line.
- Files: Editor/Editor.App/Shell.cppm, ApplicationImpl.cpp (OnStartup, OpenInstancePage, OpenGamePage, SyncPageTitles, RefreshActivePageMark), Tools/Tools.Editor/Main.cpp (window 1600x900, title "AssiduousEngine", hidden until startup finishes).
- Kind: frame.
- Opened by: launching with a project (`Tools.Editor <dir>` or `--project <dir>`), or opening a project from the Project Manager (the root view is swapped).
- Regions top to bottom: MenuBar | DockManager (grows) | StatusBar. A ToastHost overlay is added to the root (bottom-right stack).
- Default dock layout (Shell::DockDefaults): Center = Project panel (persistence id `welcome`, not closable) with document tabs docked into its tab group; Bottom split 30% of height: Assets (left, 65%) | Console (right, 35%) with Profiler as a second tab beside Console (Console in front).
- Document tabs: each page is a closable tab titled with the source instance name plus " *" when dirty (SyncPageTitles, every frame). Persistence id = instance guid; Game tabs use `game-page`, `game-page-N`. The active page gets `ActiveMark` (accent strip and ring on its tab group).
- Commands: tab close button (themed close X icon tinted from palette text at 190/255 alpha); OS close goes through ConfirmExitAllowed; panels can be dragged to dock zones or floated into real OS windows (RuntimeDockableWindowHost).
- States: dirty tab title " *"; active page ring; closing a dirty page vetoes and shows the Unsaved changes dialog (6.15); OS file drops anywhere import into the Assets panel's selected group.
- Interactions: dock-tab activation sets the active page (the subject for menus, chords, palette). Drag-and-drop of panels with dock zone indicators (toolkit).
- Drawing and icons: close X drawable on DockablePanel/DockTabGroup `close-button` pseudo; icons baked to an atlas at sizes 10..32 scaled by DPI and re-baked on DPI change.
- Problems:
  - OS window title is fixed "AssiduousEngine": never names the project, the active page or unsaved state (no SetTitle anywhere).
  - No tab context menu (Close, Close Others, Close All, Reveal in Assets, Copy Path) and no keyboard shortcut to close a tab or cycle tabs.
  - Console and Assets are closable (default `m_closable = true`) but the View menu has no item to bring them back; only Reset Layout restores them. Profiler alone has View > Profiler.
  - OS drops always import into the Assets panel's selected group, even when dropped onto a viewport or another page; no drop highlight shows where it will land.
  - Closing a page while dirty from code paths other than the tab (ClosePage) only sets a status line "Closed page had unsaved changes." with no prompt.
- Inconsistencies: brand is "AssiduousEngine" here and in the splash and manager, but Help > About says "Editor"/"About Editor".

### 1.2 Project Manager screen (launcher)
- Purpose: choose which project to work on: reopen a recent one, open a folder, or create a new one; manage the recent list (thumbnail, remove, reveal).
- Files: Editor/Editor.App/ProjectManagerView.cppm, ProjectPicture.cppm, ApplicationImpl.cpp (EnterManagerMode, OpenFromManager, CreateFromManager), Editor.Project/ProjectManager.cppm, ProjectRegistry.cppm.
- Kind: frame (replaces the main window root; the shell chrome, menu bar and status bar are not shown).
- Opened by: launching with no project argument; File > Close Project (`file.closeProject`, only registered when started in manager mode).
- Regions top to bottom (a fixed 720dp centred column, 28dp vertical padding):
  1. Header: logo drawable (44px high) or the text "AssiduousEngine" (24px), plus "engine <version>" (12px grey).
  2. Action row: [New Project...] [Open Folder...] buttons.
  3. "Recent projects" caption (13px) with List/Grid ViewModeToggles on the right (mode persisted in the registry).
  4. Scrolling list (scrollbar reserved, never overlays):
     - List mode row (a themed `panel` card): picture 76x43 | name (14px, suffix "(missing)" or "(newer engine)") over the path (11px grey, ellipsis, tooltip) | engine version column (56dp, right aligned) | [Open] (64x28, hidden when missing) | "more" IconButton (28x28).
     - Grid mode tile (220dp wide, 3 per row): picture 204x115 (click opens), name (13.5px) over a subtitle "version middle-dot missing/newer engine", "more" button.
  5. Status label (12px grey) at the bottom.
- Commands: New Project... (6.18), Open Folder... (OS folder dialog, probes for Project.xml), Open (row button or tile click), more menu (5.5), tile tooltip buttons [Open] [Show in Folder] [Copy Path].
- States: empty list: "No projects yet. Create a new project, or open an existing project folder."; missing project row dimmed (0.5 grey) with no Open; newer-engine row amber; status line for errors ("Not a project (no Project.xml there). Use New Project to start one.", "Backup FAILED - not opening.", "That directory is already a project.", "Create failed (path writable?).", "Copied the project's path.", thumbnail errors).
- Interactions: no selection model by design; one click per action; tile has an interactive tooltip (TileButton, IsTooltipInteractive) whose buttons are reachable with the pointer. Opening runs the version gate (6.19).
- Drawing and icons: project picture = thumbnail PNG from the project's editor folder or an initial letter on a quiet tile; `more` (three dots) icon with a custom rounded StateListDrawable (alpha 0.045/0.09/0.13).
- Problems:
  - No search or sort over recent projects; no last-opened date shown.
  - No keyboard path at all (no focusable list, no Enter to open, no Delete to remove).
  - Remove from List is immediate with no undo or confirm.
  - Errors go to a small grey label at the bottom of a tall column, easy to miss; no toast host on this screen.
  - New Project offers no template choice (always the starter seed) and no location memory.
  - Thumbnail can only be chosen here (and via Scene > Set as Project Thumbnail), not in Project Settings.
- Inconsistencies: grid tile tooltip offers 3 actions, the more menu 4 to 6; the more button uses a hand-made fill while Open is a theme Button; the status label pattern exists only here (main frame uses StatusBar).

### 1.3 Splash window
- Purpose: show progress during the blocking startup (device, shaders, fonts, project, pages) before any GPU UI can draw.
- Files: Editor/Editor.App/EditorSplash.cppm, EditorSplashImpl.cpp; Tools/Tools.Editor/Main.cpp (opens it, `startupProgress`, `startupFinished`); ApplicationImpl.cpp ReportStartup.
- Kind: frame (separate CPU-drawn software window, 560x300, title "AssiduousEngine").
- Opened by: launch; closed when the main window shows.
- Regions: logo PNG (Branding/assiduous-logo-splash.png) centred in the upper part | status text (15px TrueType, left) | progress bar (track + accent fill) | version (bottom right, dim).
- States: steps "Starting the graphics device" 0.05, "Loading settings" 0.12, "Loading fonts" 0.18, "Preparing the interface" 0.30, "Starting the renderer" 0.45, "Loading the editors" 0.62, "Opening the project manager" 0.80 or "Opening <dir>" 0.70, "Opening <page>" 0.72..0.98 per restored page, "Ready" 1.0. Without the logo or font it draws without them.
- Interactions: none (events are pumped so the window stays responsive).
- Problems: no cancel; a failure during startup is not shown here (it goes to the log); progress is hand-tuned constants, not measured.
- Inconsistencies: colours are its own constants (not the theme palette).

---

## 2. Menu bar and top menus

### 2.1 Menu bar (ActionMenuBar)
- Purpose: discoverable list of every editor command, grouped by area, with enabled state.
- Files: Editor/Editor.App/ActionMenus.cppm; ApplicationImpl.cpp BuildMenus, RegisterActions; Editor/Editor.Core/Actions.cppm.
- Kind: toolbar (menu bar) with dropdown context menus.
- Behaviour: generated from action `menuPath`; order declared by SetMenuOrder {File, Edit, Project, Build, Game, View, "*", Help}; domain menus (Scene, Tools) take "*". Items are rebuilt on every open (OnOpening) so enabled state is live. Items sort by menuOrder; a separator is drawn between order bands (hundreds). Nested paths become submenus.
- Problems:
  - Menu items show no shortcut text, no check marks for Toggle/Window actions (`checked` is ignored by menus: Profiler Record, Scene toggles) and no icons, although declarations carry `icon` and the chord.
  - Disabled items give no reason (the registry's NotSupported is silent).
  - No mnemonics/Alt access, no keyboard navigation documented.
- Inconsistencies: the Command Palette shows chords and menu paths, the menus do not.

### 2.2 File menu
Items in order (bands separated):
- Leading list-driven items (not actions): "New <creator label>" for each uncategorised AssetCreator; then one submenu per creator category (sorted) holding bare labels (e.g. "Primitives > Cube"). Separator.
- Save (`file.save`, Ctrl+S, enabled when the subject page is dirty). Save As... (`file.saveAs`, enabled with a page; opens 6.14).
- Save Layout (`file.saveLayout`; status "Layout saved.").
- Close Project (`file.closeProject`, manager mode only; may open 6.17). Exit (`file.exit`; may open 6.16).
- Problems: no Save All, no Open/Recent Projects (only via Close Project), no Revert; "Save Layout" lives in File but its sibling "Reset Layout" lives in View; New items create into the root or selected group with no name prompt (CreateAndOpen).
- Inconsistencies: "New X" here vs "Create > X" in the Assets background menu for the same creators; top-level uncategorised creators are prefixed "New " but categorised ones in submenus are not.

### 2.3 Edit menu
- Undo (`edit.undo`, Ctrl+Z, enabled when the page can undo). Redo (`edit.redo`, Ctrl+Shift+Z, alternate Ctrl+Y). Separator.
- Preferences... (`edit.preferences`, opens 6.10). Export Templates... (`edit.exportTemplates`, opens 6.12).
- Not in menus but registered: Discard Changes (`page.discardChanges`, page toolbar and palette only).
- Problems: Undo/Redo cannot name the command; no Cut/Copy/Paste/Delete/Select All at app level (domain menus carry some: Scene/Entity/Copy, Paste).
- Inconsistencies: Export Templates is under Edit while Export... is under Project.

### 2.4 Project menu
- Project Settings... (`project.settings`, opens 6.11). Separator.
- Add Native Code... (`project.addNativeCode`; toasts: Info "already has native code", Success "Native code scaffolded in Native/ - use Project > Build Native Module, then Reload.", Warning "Native/ already exists - not touching it...", Error). Build Native Module (`project.buildNativeModule`, a job "Native Build"; toasts on result). Reload Native Module (`project.reloadNativeModule`). Separator.
- Export... (`project.export`, opens 6.7). Separator.
- Report Resource Memory (`project.reportResourceMemory`, readOnly; writes a table to the log only).
- All enabled only with a project.
- Problems: "Add Native Code..." has an ellipsis but opens no dialog; Report Resource Memory produces no visible result unless the Console is visible; native build progress only in the status bar.

### 2.5 Build menu
- Cook All (`build.cookAll`), Rebuild All (`build.rebuildAll`). Results arrive as toasts ("Cook finished: N asset(s)." success, "Cook: N failed, M cooked (see Console)." sticky error; silent when nothing cooked).
- Problems: no Cancel Cook; no visible progress other than status bar text; the same two commands are repeated at the bottom of every Assets context menu.

### 2.6 Game menu
- Play (`game.play`, opens or reveals the primary Game tab), Play New Instance (`game.playNewInstance`, a fresh Game tab `game-page-N`).
- Problems: no shortcut by default (F5 is not bound here); no Stop at app level (the Game page owns its transport).
- Inconsistencies: three different actions are labelled "Play": `game.play`, `playback.play` (page preview transport) and Scene/Simulate/Play.

### 2.7 View menu
- Reset Layout (`view.resetLayout`; status "Layout reset to default."). Separator.
- Profiler (`view.profiler`, 200), Profiler Record (`profiler.record`, Toggle, 201), Command Palette... (`view.commandPalette`, Ctrl+Shift+P, 200). Separator.
- Screenshot Editor (`view.screenshot`, F12; toast "Screenshot saved: <path>" or error).
- Problems: Profiler Record is a Toggle but the menu shows no check; no items for Console or Assets panels; Command Palette sits in the same band as Profiler items by accident of numbering (both 200).

### 2.8 Domain menus in the "*" slot (registered outside this scope, listed for completeness)
- Scene: Animation Panel, Entity Markers, Frame Selection, Orthographic, Set as Project Thumbnail, Entity/{Create Entity, Create Child, Copy, Paste, Paste as Child, Duplicate, Delete, Create Prefab from Selection, Spawn Prefab..., Spawn Prefab as Child}, Gizmo/{Translate, Rotate, Scale, Vertex Snap, World Space}, Prefab/{Apply to Prefab, Revert Instance}, Simulate/{Start, Play, Pause, Stop}.
- Tools: Bake (navigation).

### 2.9 Help menu
- About (`help.about`) opens 6.13.

---

## 3. Status bar and notifications

### 3.1 Status bar
- Purpose: the latest one-line message: what just happened, background cook/job progress, MCP activity, project identity.
- Files: Shell.cppm (`context.OnStatus` -> StatusBar::SetText), ApplicationImpl.cpp (cook and job pumps, MCP), toolkit StatusBar.cppm.
- Kind: toolbar (single text section; toolkit `AddSection` exists but is unused).
- Content written by: every Notify (mirrors toasts), cook progress lines every frame, job progress "<title>: <step> (NN%)", MCP "MCP: <tool> done/failed", "MCP host on 127.0.0.1:<port>", "Project: <name>  (<dir>)", "Project closed.", "Preferences saved.", "Project settings saved.", "Duplicated as '...'", "Deleted N ...".
- Problems: one shared line is overwritten every frame by cook/job progress, so a just-shown result can vanish immediately; no persistent indicators (cook busy, job progress bar, MCP host on/off, dirty page count, active page name); no history (click to see recent messages); no progress bar.

### 3.2 Toasts
- Purpose: transient confirmation of results and errors.
- Files: ApplicationImpl.cpp ShowToast (~713), SubmitExportJob (sticky toast with action), toolkit ToastHost.cppm.
- Kind: overlay (bottom-right stack, newest nearest the corner; each card has a severity accent bar, message, optional action button, close button).
- States: Info/Success/Warning 5 s; Error sticky (0 s); Export complete: sticky success with [Open Folder] action.
- Problems: no cap or grouping (a cook watcher or batch import can stack many); errors say "(see Console)" but give no button to jump to the Console line; toasts cover the bottom tool pane (Console/Assets) where the user is often looking; same text is duplicated into the status bar.
- Inconsistencies: message style varies: "Save FAILED (see Console).", "Saving export presets FAILED (see console).", "Removing the template failed (see the Console).", "The screenshot failed (the Console, category Screenshot, says why)"; uppercase FAILED/NOT in some, lowercase in others; "console" vs "Console".

---

## 4. Dock panels and pages

### 4.1 Project panel without a project (welcome)
- Purpose: keep the centre tab group alive and show identity when no project page content exists.
- Files: Shell.cppm BuildWelcome.
- Kind: panel (non-closable, persistence id `welcome`).
- Content: logo (72px high, opacity 0.9) or "AssiduousEngine" (28px), and "Open an asset to begin" (13px grey), centred.
- Problems: text says "Open an asset" but offers no action; it is only visible in the CLI single-project flow edge cases (project failed to open), where it should explain the failure.

### 4.2 Project page (ProjectHomeView)
- Purpose: an overview of the open project: identity, a quick start (open default scene, play), all scenes, a read-only summary of every project setting with links to referenced assets, and the Project menu's actions.
- Files: Editor/Editor.App/ProjectHome.cppm, ProjectPicture.cppm.
- Kind: page (content of the Project panel, scrolls vertically; rebuilt when settings change, cook revision moves, or thumbnail mtime changes).
- Regions top to bottom:
  1. Header: project picture 192x108 | name (22px) / "AssiduousEngine <engineVersion>" (12.5px dim) / folder path (12px dim, tooltip).
  2. Two wrapping columns:
     - START (280dp): [Open <default scene>] or disabled [No default scene] (30dp); row of buttons for every `Game/` action (Play, Play New Instance; tooltip = description); "SCENES (N)" heading; one link per scene asset ("<name>  (default)" for the default; tooltip = path); "No scenes yet." when none.
     - PROJECT (440dp): per settings category a title (13px), then rows "Label (150dp dim) | value" where an asset setting is a link (or "(missing)" orange, or its empty text dim), lists are stacked links, enums by name, bools On/Off, numbers, strings or "(none)"; then a row of buttons for every `Project/` action.
- Commands: links open assets (OnOpenAsset); action buttons run registry actions with a null subject.
- States: "No project is open."; missing assets orange; disabled buttons per registry.
- Problems: settings are read-only here with no "Edit..." jump to the setting in Project Settings; action buttons' enabled state is only refreshed on rebuild; scene list has no filter or create button; links look like plain text until hovered.
- Inconsistencies: section headings are uppercase "START", "PROJECT", "SCENES (N)" while category titles and every other panel use title case; label column 150dp vs 110dp in the dialogs; MSAA shown via the render module's labels while other enums show raw enumerator names.

### 4.3 Assets panel (AssetsView, the asset browser)
- Purpose: browse and manage the project's source content database: groups (folders) and asset instances with their type and cook state; open, create, import, rename, duplicate, delete, cook, flag for export, pin favorites; drag assets into slots.
- Files: Editor/Editor.App/AssetsView.cppm, AssetsViewImpl.cpp, AssetFilter.cppm, AssetDragData.cppm, ViewModeToggles*.cpp*, EditorIcons*.cpp*, ImportDialog.cppm, GroupPickerDialog.cppm; Editor.Core/SearchFilter.cppm.
- Kind: panel (persistence id `assets`; until a project opens it shows the placeholder label "Asset browser (phase 6)").
- Regions: horizontal SplitView 30/70:
  - Left: group tree (TreeView, 22px rows, NameLabel 13px; root shown as "Content"; all groups expanded on rebuild).
  - Right column (padding 6x4, spacing 4): header row 26dp = BreadcrumbBar ("Content / a / b", segments clickable) | List/Grid ViewModeToggles; filter EditText (placeholder "Filter all assets..."); content as ListView (22px rows, multi-select) or GridView (96x84 cells, multi-select).
  - List row: type icon 16 | name (NameLabel 13px, colour = cook state) | meta label "TypeName  [cooked]/[not cooked]/[FAILED]" or "Group" | corner dots (green = Always Export, gold = favorite).
  - Grid tile: 40x40 thumbnail (else type icon) over the name (12px centred, ellipsis), corner dots; no meta label (cook state only as name colour).
  - Subgroups list first (gold-ish name, folder icon), then instances.
- Commands: context menus 5.1 to 5.3; F2 inline rename; Delete key (confirmed); double-click opens an asset or descends into a group; breadcrumb climbs.
- States: cook badges (Cooked green, Missing yellow, Failed red, NoBuilder none); thumbnails stream in per tile (RefreshThumbnail); mutation during a cook queues with an Info toast ("Import/Rename/Delete/New group queued until the current cook finishes."); a whole guid typed in the filter selects that asset.
- Interactions:
  - Selection: Ctrl/Shift multi-select in list and grid; selection clears on every rebuild and filter change.
  - Inline rename: slow-click, F2, or menu Rename (double-deferred focus); rejects / \ : * ? " < > |; the root is not renamable; errors as toasts ("NOT renamed: name already taken.").
  - Filter: name contains (any case) or guid prefix; searches ALL groups and ignores the selected group; navigating to a group clears the filter.
  - Drag source: instance rows/tiles (AssetCell IDragSource) carry AssetDragData (guid, type, name); groups do not drag.
  - Drop target: none (OS file drops are handled by the app, not by this view).
  - Reveal(id): navigate to the owning group, select and scroll to the row (used by slot reveal buttons).
  - List/grid mode persisted per project (EditorAssetBrowserSettings).
- Drawing and icons: AssetCell::OnDraw paints 3px corner dots right-to-left; icons via EditorIcons::ForAssetType (scene, prefab, mesh, skinned mesh, material, texture/image, particle, animation clip, animation graph, skeleton, folder, unknown).
- Problems:
  - No Enter to open the selected asset (ListView handles only arrows/Home/End/PgUp/PgDn; F2 and Delete are added); the code comment claims Enter opens a guid-selected row.
  - Cannot move assets between groups (no drag onto a group, no Move To...), cannot drag several assets at once, cannot drop OS files onto a specific group.
  - No sort options, no type filter, no favorites-only view, no column headers, no count footer.
  - Empty states missing: an empty group or a filter with no matches shows a blank area; no "no project" state other than the placeholder text.
  - Filter has no clear button or magnifier, and silently widens to all groups.
  - Group tree has no folder icons and no counts; the tree right-click selects the group and shows the background menu, so tree and content menus differ in subtle ways.
  - Multi-select actions only cover Cook/Rebuild/Delete; Duplicate, Copy GUID, Copy Path, Pin and Always Export act on the clicked row only.
  - Many asset types (Font, Script, Audio clip, Sound cue, Bus layout, Input map, UI theme/document, GUI sheet/document, Shader, Terrain, Heightfield, Collision shape, Vegetation, Settings profile) get the generic "unknown" glyph in list rows until a thumbnail exists.
  - Drag ghost is the default grey rounded rect with no icon or name.
- Inconsistencies: favorites shown as gold dots here but "* " prefix and gold text in the Asset Picker; cook state is text + colour in list mode but colour only in grid mode; group tree row height 22 vs 20 in every picker; filter placeholder differs from the picker's.

### 4.4 Console panel (LogView)
- Purpose: read the editor's log (level, category, message), filter it, copy lines.
- Files: Editor/Editor.App/LogView.cppm; ApplicationImpl.cpp DrainLog, OnCopied toast; Editor.Project/LogBuffer.cppm.
- Kind: panel (persistence id `console`).
- Regions top to bottom: header 28dp: four level chips (Debug, Info, Warning, Error: coloured dot 8px, name 12px, count 11px capped "9999+"; pill toggles) | spacer | magnifier icon 14 | filter EditText ("Filter lines", tooltip, 80 to 260dp) | clear-filter X (hidden until text; keeps its place) | trash IconButton "Clear the console"; hairline Separator; ListView of 20px rows (padding 8x4), each "[Category] message" in the level colour (Debug grey, Info blue, Warning yellow, Error red).
- Commands: context menu 5.4; Ctrl+C copies selected rows (toast "Copied N log lines"); Ctrl+A selects all visible; Clear.
- States: level chip dims when off or empty; MaxEntries 1000 (oldest trimmed, selection shifted); AutoScroll always on.
- Interactions: multi-select (click, Ctrl, Shift); filter is case-insensitive substring including the category; level/filter change clears the selection.
- Problems: no timestamps; no way to pause auto-scroll (it jumps to the newest line while you read); no category filter other than text; no wrap or detail view for long messages; no "jump to source" or click-through from toasts; no empty state ("No messages"); 1000-line cap is silent.
- Inconsistencies: only search field in the editor with a magnifier and a clear button; trash icon here means "clear all", elsewhere "remove row".

### 4.5 Profiler panel
- Purpose: record and inspect the editor process's frame timings (CPU scopes, GPU passes, counters) over a selected frame range; save a capture; open a player's capture.
- Files: Editor/Editor.App/ProfilerPanel.cppm; Foundation/Profiler.UI/FrameTimeGraph.cppm, Timeline.cppm, ScopeTable.cppm, CounterStrip.cppm; Shell.cppm (tab beside Console; closing the tab stops recording); PathPickerDialog.cppm.
- Kind: panel (persistence id `profiler`).
- Opened by: View > Profiler (`view.profiler`), View > Profiler Record (`profiler.record`, shows the tab and toggles), the Profiler tab.
- Regions top to bottom (body min 640x360; scrolls in both axes when the pane is smaller):
  1. Toolbar: [Record] toggle | [Clear] | sep | [Open...] [Save] [Live] (Live only while viewing a capture; Save disabled then) | sep | status label (tooltip explains editor-vs-player numbers).
  2. Frame strip (FrameTimeGraph, fixed height): CPU area, GPU line, 16.7/33.3 ms budget lines; click selects a frame, drag selects a range; follows the newest frame while recording until the user selects.
  3. Timeline (left, width minus side column): lane per thread, scopes stacked by depth, GPU lane, frame edges; wheel zooms about the pointer, drag pans, Shift+wheel scrolls lanes; hover names a bar "name  N ms  CPU/GPU"; highlights the name selected in the table.
  4. Side column (40% of width, 260 to 380dp): ScopeTable (Name, Total ms, Self ms, Count; click heading sorts, again reverses, " v"/" ^" markers; click row selects and highlights in the timeline; wheel scrolls) over CounterStrip (two columns of counters: per-frame means and held values; counters no frame set are hidden).
- Status texts: "Editor process (every page, preview and Play-in-editor run): Record to start", "Recording the editor process: N frames", "Editor process: N frames", "<path>: N frames of <app> (<platform> <build>)", "Nothing to save: record first", "Saved N frames to <path>", "Could not save to <folder> (see the Console)", "Could not read <path>".
- Interactions: Open... shows PathPickerDialog over <user-data>/profiles filtered to ".aeprofile".
- Drawing: all four widgets are custom OnDraw; ScopeTable draws its own headings.
- Problems: empty strip/timeline/table draw nothing (no in-widget empty state); Save gives no choice of name or folder; Open lists bare relative filenames with no dates, sizes or sorting; no keyboard (arrows to step frames, Esc to clear selection); no "frame N of M" readout near the strip; Clear has no confirm.
- Inconsistencies: uses toolkit Toolbar text buttons while the Console header uses icon buttons; status shown in a toolbar label rather than the status bar.

### 4.6 Document page host (UIEditorPage) and the page toolbar
- Purpose: the contract every asset page meets to dock as a tab; the PageToolbar is the standard per-page command strip.
- Files: Editor/Editor.App/UIPage.cppm, PageToolbar.cppm.
- Kind: page host and toolbar.
- PageToolbar regions: Standard::Edit = [Save] | [Undo] [Redo] | [Discard Changes]; Standard::Save = [Save]; Standard::None = nothing; AddPlayback appends | [Play] (toggle, checked while playing) [Stop] [Restart]; pages add domain actions by id. Refresh() each frame syncs enabled/checked over THAT page.
- Problems: buttons are text only (labels) with no icons and no tooltips (the declaration's description and chord are not used); a page that asks for an unknown id silently gets nothing; Discard Changes has no confirm.
- Inconsistencies: menu Save acts on the active page while the toolbar Save acts on its own page (deliberate, but nothing tells the user which page the menu will hit apart from the tab ring).

### 4.7 Viewport tool panel seam (ToolPanel)
- Purpose: mount a settings panel for the active viewport tool (terrain sculpt/splat, vegetation paint, navigation bake, property animation) beside the viewport.
- Files: Editor/Editor.App/ToolPanel.cppm, ToolPanelImpl.cpp, ToolPanelWidgets.cppm.
- Kind: panel seam (placement hint Dock, Float, ViewportOverlay; the scene page decides).
- Widget kit (ToolPanelWidgets): MakeToolPanelRoot (vertical, spacing 4), MakeToolPanelRow (caption label), SegmentedToggle (exclusive toggle row, optional tooltips), AddToolPanelFloat/Bool (property grid rows), AddToolPanelGrid (fills).
- Problems: panels are recreated on every activation so scroll position and expanded state reset; no common header (tool name, close/deactivate, help) is provided by the kit.

---

## 5. Context menus

### 5.1 Assets: instance row menu (list or grid)
Open | sep | Rename, Duplicate | sep | Copy GUID, Copy Path, Pin favorite / Unpin favorite, Always Export / Remove from Always Export | sep | Cook, Rebuild | sep | Delete (or "Delete N assets" when the right-clicked row is part of a multi-selection).
- Feedback: Copy shows a toast "Copied GUID"/"Copied path"; Always Export toast "Always Export: <name>"; Duplicate status line "Duplicated as '<name>'".
- Problems: only Cook/Rebuild/Delete honour multi-select; no Reveal in OS, no Find References, no Reimport, no Move To; no shortcuts shown.

### 5.2 Assets: subgroup row menu
Open | sep | Rename, Always export contents / Don't always export contents | sep | Cook Group, Rebuild Group | sep | Delete Group.

### 5.3 Assets: background and group tree menu
Create > (uncategorised creators, sep, one submenu per category) | New Group | Import... | sep | (non-root) Rename Group, Always export contents / Don't always export contents; Cook Group, Rebuild Group; (non-root) Delete Group | sep | Cook All, Rebuild All.
- Inconsistencies: "Rename" in the row menu vs "Rename Group" here for the same object; "Always Export" (title case, instance) vs "Always export contents" (sentence case, group); "Pin favorite" lowercase f; "Create >" vs File's "New X".

### 5.4 Console menu
Copy (or "Copy N Lines"; disabled with nothing selected) | Select All (disabled when empty) | sep | Clear.

### 5.5 Project Manager card "more" menu
Open (not when missing) | Show in Folder | Copy Path | Choose Thumbnail Image... (not when missing) | Clear Thumbnail (only when one exists) | sep | Remove from List.

### 5.6 ContainerListEditor add menu
Filled by the consumer (OnAddMenu, add-by-kind); shown at the add icon.

---

## 6. Dialogs

### 6.1 Command Palette (CommandPaletteDialog)
- Purpose: run any action by name from the keyboard.
- Files: Editor/Editor.App/CommandPalette.cppm.
- Opened by: View > Command Palette... (`view.commandPalette`, Ctrl+Shift+P).
- Size: 520..720 x 360..520.
- Regions: filter EditText ("Type an action's name...", focused) | ListView rows 26px: label (13px, grey when disabled) | menu path (11px grey) | chord (12px, fixed 110dp) | status line (11px grey: "Nothing matches.", "'<label>' is not available over the active page.").
- Commands: Up/Down move the highlight (captured before the field), Enter/Keypad Enter run, double-click runs, Escape closes. Ranking: label prefix, label contains, then description or id contains.
- Problems: no recent/most-used ordering; description not shown; Toggle state not shown; disabled actions are listed and only refused after Enter; actions with no menu path (Discard Changes, playback.*) show an empty location, so the three "Play" rows are hard to tell apart; no fuzzy matching.
- Inconsistencies: the file comment says "label | chord | menu path" but the row is label | path | chord; the only dialog with Enter-to-confirm and arrow-key list navigation.

### 6.2 Asset Create dialog (AssetCreateDialog)
- Purpose: choose a group and a name for a new asset (type-agnostic; caller creates).
- Files: Editor/Editor.App/AssetCreateDialog.cppm. Consumer: Editor.PropertyAnimation (Create Clip).
- Size: 420..520 x 360..460. Title and placeholder supplied by the caller.
- Regions: group tree (20px rows, 12px labels, "Content" root, expanded) | name EditText | validation line (11px, uncoloured): "no group selected", "enter a name", "'<name>' already exists in this group", "create in <group>" | [Create] [Cancel].
- Problems: Create stays enabled when invalid (click silently does nothing); the preselected root is not visibly selected in the tree; no check for filesystem-hostile characters; no Enter to create.
- Inconsistencies: validation lowercase, no colour, live; Save As uses red "NOT saved: ..." on click; New Project validates silently.

### 6.3 Asset Picker dialog (AssetPickerDialog)
- Purpose: choose one asset of the accepted types for a reference field, or clear it.
- Files: Editor/Editor.App/AssetPickerDialog.cppm, AssetPickerDialogImpl.cpp.
- Opened by: clicking an AssetPickerSlot body (ResourceRefEditor::BindAsset), ContainerListEditor slot pick, Project Settings asset rows and lists.
- Title: "Select asset". Size 520..640 x 380..460.
- Regions: header = filter ("Filter all groups...") | List/Grid toggles (mode persisted per project); hint line (orange, hidden unless a typed guid names an asset of the wrong type: "'<name>' is a <type>, which this slot does not take"); SplitView 32/68: group tree (20px) | list (28px rows: thumbnail/type icon, "* " + full path + "   -   " + type; favorites gold) or grid (96x84, 48px thumbnail, name; tooltip path - type) | [Select] [Clear] [Cancel].
- Interactions: favorites of a matching type listed first regardless of group; filter across all groups; double-click confirms; right-click row: Pin favorite / Unpin favorite; thumbnails update live via a listener.
- Problems: the field's current value is not preselected or scrolled to; no Enter to select; Select with nothing selected does nothing silently; empty result shows a blank list (no "No <type> assets yet" + create shortcut); Clear closes immediately with no undo hint; the list shows full paths (long, repetitive) while the tree already shows the group.
- Inconsistencies: favorites marked "* " here vs gold dot in the browser; list row 28 vs 22 in the browser; grid icon 48 vs 40; title sentence case "Select asset" vs "Select destination group".

### 6.4 Confirm dialog (ConfirmDialog) and Dialog::Confirm
- Purpose: ask before a destructive or branching action.
- Files: Editor/Editor.App/ConfirmDialog.cppm (2 to 3 labelled choices, OnChosen(index), Escape = last choice; 340..460 wide, 12px wrapped message); Foundation/UI/Overlay/Dialog.cppm `Dialog::Confirm` (OK / Cancel).
- Uses: ConfirmDialog: Remove Template ("Remove" / "Cancel"), property-animation dirty guard (Save / Discard / Cancel). Dialog::Confirm: "Delete assets" ("Delete '<name>'? Its source file and cooked product go away; open pages close." or "Delete N assets? ..."), "Delete group" ("Delete group '<name>' and ALL its contents (N asset(s))? ...").
- Problems: Delete confirms use generic "OK" instead of a verb, no destructive styling, no list of the assets to be deleted, no "references will break" warning; no default/focused button and Enter does nothing.
- Inconsistencies: three confirm styles coexist (Dialog::Confirm OK/Cancel, ConfirmDialog verbs, hand-built ui::Dialog with verb buttons in ApplicationImpl).

### 6.5 Group Picker dialog (GroupPickerDialog)
- Purpose: choose a destination content group (import destination).
- Files: Editor/Editor.App/GroupPickerDialog.cppm. Opened by Import dialogs' [Change...].
- Title "Select destination group". Size 420..520 x 340..420.
- Regions: group tree (20px, 12px labels, all expanded) | [Select] [Cancel]. Single click selects, double-click confirms.
- Problems: the preselected (current) group is stored but not highlighted in the tree; no New Group button; no filter for deep trees; no folder icons.
- Inconsistencies: the fourth copy of the same group-tree adapter (browser, asset picker, create, group picker), each slightly different.

### 6.6 Path Picker dialog (PathPickerDialog)
- Purpose: choose an existing file under a root folder, filtered by extension (font source files, profiler captures).
- Files: Editor/Editor.App/PathPickerDialog.cppm. Opened by: Profiler Open... ("Open a capture"), font page source pickers.
- Size 460..560 x 340..420.
- Regions: filter ("Filter...") | ListView 20px rows of root-relative paths (12px) | [Select] [Cancel].
- Problems: rows are in directory-walk order (unsorted); no empty state ("No .aeprofile files in <folder>"); no file size/date; Select with nothing selected is a silent no-op; no Enter or arrow navigation from the filter; no icon per file.
- Inconsistencies: filter placeholder "Filter..." vs "Filter all assets..." vs "Filter lines".

### 6.7 Export dialog (ExportDialog)
- Purpose: manage the project's export presets (per platform: template, config, player name, output, extra files, symbols, pruning, display overrides) and run an export.
- Files: Editor/Editor.App/ExportDialog.cppm, ExportDialogImpl.cpp; Editor.Project/ExportPreset.cppm, ExportController.cppm, ExportTemplate.cppm, TemplateIcons.cppm.
- Opened by: Project > Export... (`project.export`; "Open a project first." toast without one).
- Title "Export". Size up to 820 x 520 fixed height.
- Regions: left 250dp: "Presets" heading + IconButtons add (plus), duplicate (copy), remove (trash) | preset list (40px rows: template icon, name, warning when no template resolves) | "Exports go to <project>/Dist". Vertical rule. Right: resolve card ("Exports with <template>", or "No presets yet / Add one with + for this platform.", "Template '<id>' is not installed here", "No template for <platform> <config> here / Install or create one in Export Templates, or pick another platform.", plus "Built for another engine version; the export still runs.") over CategoryTabs:
  - General: Name, Template (combo: "Any for its platform and config" + templates), Platform, Config (default Release), note, Player name (placeholder "the template's (Engine.Player)"), Output folder (placeholder "the preset's name").
  - Content: Extra files (text "none (separate files with ;)" + [Add Files...]), note, "Ship the template's debug symbols", "Ship only the content the game reaches" (tooltip), "Include shader source" (tooltip).
  - Display: note, "Draw at its own render size" + Render size (W x H) + Render fit; "Open its own window" + Window size + Window mode + "Resizable"; "Held its own way (orientation)" + Orientation.
  - Buttons: [Export Templates...] [Export All] [Export] [Close].
- Commands: Export runs the selected preset and closes; Export All runs all and closes; progress then appears only in the status bar ("Cooking before export..." toast, job "Export"/"Export All"), result as a sticky toast with [Open Folder].
- States: edits apply immediately and save on selection change, export, or close; "An export is already in progress." toast.
- Problems: Remove preset has no confirm and no undo; extra files are a semicolon-separated text field; once Export is clicked the dialog closes and there is no export progress window or cancel; no validation of the output folder name; no last-export result per preset.
- Inconsistencies: auto-saving while Preferences and Project Settings require Save; list IconButtons at 14px while ContainerListEditor uses default-size IconButtons; Templates' remove confirms but preset remove does not; "Close" here and in Templates vs "Cancel" in other dialogs.

### 6.8 Import dialog, single file (ImportOptionsDialog)
- Purpose: review what an import will create (resource plan per kind with enable checkboxes and editable names), choose the destination group and importer options, then import.
- Files: Editor/Editor.App/ImportDialog.cppm (ImportPlanView, ImportOptionsDialog); AssetsViewImpl.cpp ImportWith, ShowImportReview, ExecuteImport, CommitImport.
- Opened by: Assets background menu Import... (OS file dialog), OS file drop, with exactly one file and one importer; heavy importers (models) open it only after a worker prepare ("Reading <file>" in the status bar); importers with no options and no plan import immediately with no dialog.
- Title "Import". Max 620x700 with a plan, else 480x420.
- Regions: "Source:" file name (11px, 52dp label column) | "Into:" group path + [Change...] (opens 6.5) | resource plan (scroll): per kind (Mesh, Material, Texture, Skeleton, Animation clip, Collision) a check-all header checkbox then rows [check 38dp][name EditText] | one checkbox per importer Toggle (tooltip = description) | [Import] [Cancel].
- States: re-import memory merges a previous selection; "Import queued until the current cook finishes." toast; failures "Import failed: '<file>' (see Console)."; success "Imported '<name>'".
- Problems: for heavy files nothing visible happens between the drop and the dialog except a status line; no name-collision warnings in the plan; an emptied name silently keeps the old one; no preview or thumbnail; no per-kind counts; check-all header does not reflect a partially checked state.
- Inconsistencies: "Source:"/"Into:" labels with colons and a 52dp label column while Preferences/Settings/Export use 110dp columns without colons.

### 6.9 Import dialog, batch (BatchImportDialog)
- Purpose: review a whole drop of files in one session: per file enable, importer choice when ambiguous, plan and options.
- Files: Editor/Editor.App/ImportDialog.cppm; AssetsViewImpl.cpp ShowBatchImport.
- Opened by: several files dropped/picked, or any file with more than one importer.
- Title "Import Files". Max 860x700.
- Regions: "Into:" path + [Change...] | split: left 300dp scroll of BatchFileRow (enable checkbox 30dp, file name 12px, importer ComboBox 110dp when ambiguous; selected row drawn with accent fill at 0.35 alpha) | right scroll detail: file name (13px), "Reading file..." until described, then plan view and option checkboxes | [Import] (disabled until every enabled file is described) [Cancel].
- States: a file whose worker prepare failed is unchecked and an error toast appears; files without an importer are dropped before the dialog with a warning toast "No importer for '<file>'.".
- Problems: the user is not told why Import is disabled; no per-file status icon (reading, ready, failed); dropped files without importers are only toasts; options are per file with no "apply to all".
- Inconsistencies: selection is custom-drawn here while every other list uses the ListView selection look.

### 6.10 Preferences dialog (EditorPreferencesDialog)
- Purpose: edit per-user editor preferences (fonts, UI scale, view behaviour, templates root, MCP agent access, Game tab preview resolutions, keyboard shortcuts, domain contributions).
- Files: Editor/Editor.App/PreferencesDialog.cppm, CategoryTabs.cppm, ShortcutCapture.cppm; Editor/Editor.Core/EditorSettings.cppm (EditorFontSettings, EditorUiSettings, EditorMcpSettings, EditorShortcutSettings, GamePreviewSettings), Editor.Core/Context.cppm (EditorSettingsContribution); contributors Editor.Play/GamePageImpl.cpp, Editor.Navigation/NavigationBakeImpl.cpp.
- Opened by: Edit > Preferences... (`edit.preferences`).
- Title "Preferences". Fixed 680x480. Tabs (CategoryTabs, each page padded 12x10 and scrolled):
  - Appearance: "UI font (.ttf)" text (placeholder "built-in (embedded fallback)"); "Mono font (.ttf)" text ("built-in"); "UI scale" Slider (min..max step 0.05) + percent label (initial text "1.00x" then "NN%"); note "Font changes apply on restart."; checkbox "Axis views are orthographic" (tooltip).
  - Export: "Templates root" text (placeholder = default root); note about $ENV_TEMPLATES_DIR.
  - Agent access: checkbox "Serve the open project to agents (MCP)" (tooltip); "MCP port" text; "MCP token" text (placeholder "minted on first enable"); note about <user-data>/mcp-token.
  - Game preview: note; rows [name EditText][width NumericField 80] x [height NumericField 80][Remove] (seeded HD, Full HD, QHD, Steam Deck, Phone portrait, Phone landscape); [Add resolution] adds "Custom 1920x1080".
  - Shortcuts: note "Click a chord and press the new keys (Esc cancels, Del clears). A chord another action holds is refused on Save, naming it."; one row per action: label (170dp) | menu path (grey, grows) | ShortcutCaptureButton (150dp) | [Reset] (60dp, enabled when overridden).
  - Domain tabs: "Game audio" ("Hear every Game tab"), "Navigation" ("Parallel navmesh bake"), checkboxes with tooltips.
  - Buttons: [Save] [Cancel].
- States: Save applies UI scale live (re-bakes icons) and restarts/stops the MCP host; warnings "MCP port must be a number in 1024..65535 - kept the old one.", "Shortcut kept: <chord>: '<label>' holds it"; status "Preferences saved." or error toast.
- Problems:
  - Domain-contributed checkboxes write immediately on toggle (not staged), so Cancel does not undo them, unlike every other field.
  - Shortcut conflicts are detected only on Save, after the fact, as toasts; no conflict indicator in the row; no search over 50+ actions; no grouping by menu; actions without a menu path show an empty location.
  - Font paths and templates root are free text with no Browse button and no validation.
  - MCP token shown in clear text with no copy or regenerate button; port is a text field, not a number field.
  - UI scale only applies on Save (no live preview while dragging); no Apply button; no Restore Defaults per tab.
  - Removed preview rows are hidden but there is no undo; empty-named rows are silently dropped on Save.
- Inconsistencies: Preview rows use a text "Remove" button while lists elsewhere use the trash icon; scale label starts as "1.00x" then switches to "%"; label column 110dp but shortcut label 170dp.

### 6.11 Project Settings dialog (ProjectSettingsDialog)
- Purpose: edit the project manifest (Project.xml): name, native module, startup assets, audio and UI defaults, display and window settings.
- Files: Editor/Editor.App/SettingsDialog.cppm, SettingsDialogImpl.cpp; Engine/Engine.Project/ProjectSettingsImpl.cpp (labels and categories by reflection), ProjectModule.cppm.
- Opened by: Project > Project Settings... (`project.settings`).
- Title "Project Settings". Fixed 620x420. Tabs in order of first appearance (each row: 110dp label | editor):
  - General: Name (text), Native module (text), Engine version (read-only label).
  - Startup: Default scene, Startup script, Default input map, Loading screen (empty "(built-in)"), GUI loading screen (empty "(the UI's)") as asset slots (pick, drop, clear; Edit and Reveal disabled).
  - Audio: Default bus layout (slot, empty "(built-in)").
  - UI: Default UI theme ("(built-in)"), Default UI font ("(built-in)") slots; Other UI fonts and GUI sheets as ContainerListEditor slot lists (empty slot text "(pick a <Type>)").
  - Display: Render width, Render height (NumericField 100dp, range 0..16384), Render fit (combo of enumerator names), Window width/height (1..16384), Window mode (combo), Window resizable (checkbox with empty label), Orientation (combo), MSAA (combo from the render module's labels, appended last).
  - Buttons: [Save] [Cancel].
- States: status "Project settings saved." then settings-derived state re-applies; error toast "Project settings save FAILED (see console)."; duplicate and nil list entries are dropped on Save.
- Problems: "Engine version" shows the running editor's version (kEngineVersionString), not the project's stamp; Native module is free text with no browse or validation; enum combos show raw enumerator names; no per-field help text; the window/render 0x0 meaning ("output size") is not explained; checkbox rows have an empty checkbox label beside a left label (two different checkbox idioms in one dialog vs Preferences' labelled checkboxes); no way to set the project thumbnail here.
- Inconsistencies: MSAA lands at the end of Display instead of its reflection position; Edit/Reveal slot verbs removed here but present in inspectors.

### 6.12 Export Templates dialog (TemplatesDialog)
- Purpose: manage the export templates (player builds per platform/config) this editor exports with: inspect, install from a folder, create from a build, reveal, remove.
- Files: Editor/Editor.App/TemplatesDialog.cppm, TemplatesDialogImpl.cpp; Editor.Project/ExportTemplate*.cpp*, TemplateIcons.cppm (built-in SVG icons desktop, windows, linux, handheld, web, phone; Win64 -> windows, Linux64 -> linux, Web -> web).
- Opened by: Edit > Export Templates... (`edit.exportTemplates`); Export dialog [Export Templates...] (over it; Export refreshes on close).
- Title "Export Templates". Fixed 760x460.
- Regions: list 290dp (42px rows: template icon 26 | name 12.5 / summary "Platform middle-dot Config middle-dot version middle-dot this editor's own" 11px dim | warning icon when engine mismatch, tooltip "Built for another engine version") | vertical rule | details scroll: empty note "No templates. Install one from a folder, or create one from a build." or heading (icon 44, name 16px, id dim) + rule + rows Platform, Config, Compiler (hidden if empty), Engine (+ warning icon, "(this editor is X; an export still runs)"), Source ("This editor's own player build (always available)" / "Installed in the templates folder"), Folder, Player, Runtime files, Symbols, Notes (hidden if empty); [Reveal Folder] [Remove...] (disabled for the host build, tooltip says why) | footer [Install from Folder...] [Create from Build...] [Close].
- States: toasts "Installed template '<id>'.", "That folder holds no template (no valid template.xml).", "Created template '<id>'.", "No player build there: pick a Bin/<Config>/<Platform>-<Compiler> folder holding Engine.Player.", "Removed template '<id>'.", error on remove.
- Problems: install/create copy synchronously on the main thread with no progress; no keyboard delete; empty values show "-"; no download/update path.
- Inconsistencies: action buttons split between the details pane (Reveal, Remove) and the footer (Install, Create); "Close" vs "Cancel".

### 6.13 About dialog
- Purpose: show the editor version/build.
- Files: ApplicationImpl.cpp (`help.about`, hand-built ui::Dialog).
- Content: "About Editor" title, "Editor" (18px), "Version <BuildStamp>" | [OK].
- Problems: no engine version string, no copy-to-clipboard, no links (docs, licences).
- Inconsistencies: names the product "Editor" while everything else says "AssiduousEngine".

### 6.14 Save As dialog
- Purpose: save the active page as a new asset next to the original.
- Files: ApplicationImpl.cpp SavePageAs.
- Opened by: File > Save As... (`file.saveAs`).
- Content: "New name (created next to '<name>'):" | name EditText (prefilled "<name> Copy", repeated until unique) | red error line (empty until a failed attempt) | [Save] [Cancel].
- States: "NOT saved: enter a name.", "NOT saved: that name already exists in the group." (dialog stays); toasts "Saved as '<name>'." or failure.
- Problems: cannot choose a different group (unlike Asset Create); validation only on click; no Enter to save; name text not preselected for quick overwrite.
- Inconsistencies: red uppercase "NOT saved:" validation vs Asset Create's lowercase live line.

### 6.15 Unsaved changes: closing one page
- Files: ApplicationImpl.cpp ShowDirtyCloseDialog.
- Opened by: closing a dirty page tab (OnCloseInterceptor veto).
- Content: "'<title>' has unsaved changes." | [Save] [Discard] [Cancel].
- Problems: no Enter default; on save failure the dialog closes and a toast says "Save FAILED (see console) - page stays open.".

### 6.16 Unsaved changes: exit
- Files: ApplicationImpl.cpp ConfirmExitAllowed.
- Opened by: File > Exit, OS close button.
- Content: "N page(s) has/have unsaved changes." | [Save All & Exit] [Exit Without Saving] [Cancel].
- Problems: does not list which pages are dirty; no per-page choice.

### 6.17 Unsaved changes: close project
- Files: ApplicationImpl.cpp ConfirmCloseProjectThen.
- Content: same count message | [Save All & Close Project] [Close Without Saving] [Cancel].
- Inconsistencies: three near-identical "Unsaved changes" dialogs with different verb phrasings ("Discard" vs "Exit Without Saving" vs "Close Without Saving"), each built by hand.

### 6.18 New Project dialog
- Purpose: create and open a project (name + parent directory).
- Files: ProjectManagerView.cppm ShowCreateDialog.
- Title "New Project". MinWidth 460.
- Content: name EditText (placeholder "Project name") | row: parent directory EditText (placeholder "Parent directory (the project is created inside it)") + [Browse...] | [Create] [Cancel].
- States: errors after the fact in the manager status line.
- Problems: Create with an empty field silently does nothing; the resulting full path is not previewed; no template/starter choice; no name character validation; directory not remembered.

### 6.19 Project open version gate
- Purpose: protect a project when its engine stamp differs from this editor's.
- Files: ApplicationImpl.cpp OpenFromManager; Editor.Project/ProjectManager.cppm (DecideOpen supplies title and body text).
- Variants: newer engine: [Open Anyway] [Cancel]; older engine: [Back Up && Open] [Open Without Backup] [Cancel] (the label source uses "&&", which the toolkit shows as a literal "&" or "&&" depending on menu-mnemonic handling; the exit dialog uses "&" in "Save All & Exit").
- Problems: the backup location is not shown before or after; failure only in the manager's status label.
- Inconsistencies: "&&" in these labels and in "Save All && Close Project" vs single "&" in "Save All & Exit".

---

## 7. Shared inline widgets and editors

### 7.1 Asset reference slot (AssetPickerSlot)
- Purpose: show and change one asset (or entity) reference: name, preview, pick, edit, reveal, clear, accept a drop.
- Files: Editor/Editor.App/AssetPickerSlot.cppm.
- Kind: inline widget.
- Layout left to right: body Button with the asset name (grows; "(none)" or the row's empty text; tooltip "Choose asset"; click = pick) | preview ContentButton (14px thumbnail or type glyph; tooltip "Reveal in asset browser") | Edit ContentButton (pen icon, "Edit asset") | Clear ContentButton (close X, "Clear reference"). Affordances appear only when wired; Edit/Clear/Reveal disable on an empty slot.
- Drag and drop: drop target when accepted types are set (asset types, "*" any asset, "@entity" hierarchy rows); hover draws a 2px ring: accent when the type matches, error colour when not; a wrong-type drop logs and calls OnRejectedDrop ("<name> is a <type> - this field takes <A>, <B> or <C>").
- Problems: a dangling reference shows the "(missing)" name text with no warning colour or icon; clicking the name opens the picker without preselecting the current value; no keyboard: Delete to clear or Enter to pick; the body is a full button so the name cannot be selected/copied.
- Inconsistencies: Clear uses the same close X as tab close and search clear.

### 7.2 Compact asset slot (CompactAssetSlot)
- Purpose: an asset slot for toolbars and preview bars (caption + slot at bar height).
- Files: CompactAssetSlot.cppm, CompactAssetSlotImpl.cpp.
- Layout: caption Label (12px) | ResourceRefEditor slot (12px body).

### 7.3 Resource reference row (ResourceRefEditor)
- Purpose: the property-grid row over an AssetPickerSlot, binding pick, drop, clear, edit and reveal to one undoable assignment.
- Files: ResourceRefEditor.cppm, ResourceRefEditorImpl.cpp.
- Behaviour: BindAsset opens AssetPickerDialog filtered by accepted types; drop assigns; clear assigns nil; Edit opens via EditorContext::OpenAsset; Reveal via RevealAsset; wrong-type drop -> Warning toast. BindEntity: the caller's entity picker; no rejection toast.
- Inconsistencies: entity-bound rows give no feedback on a refused drop while asset-bound rows toast.

### 7.4 Container list editor (ContainerListEditor)
- Purpose: the one editable list (add, reorder, remove, drop to append) for any list property.
- Files: ContainerListEditor.cppm, ContainerListEditorImpl.cpp.
- Kind: inline widget (property-grid row).
- Layout: header row: (section lists) "N item(s)" 11px or spacer | add IconButton (plus, "Add"; menu when OnAddMenu). Slot elements: per row [AssetPickerSlot 12px grows][move up][move down][remove] IconButtons (chevrons, trash; up disabled on first, down on last). Section elements: the row is header only; ElementActions() supplies move/remove icons for each element's grid section header.
- Drag and drop: slots accept typed drops (assign); the column (header, gaps, empty list) is an append drop zone with the same accent/error ring.
- Problems: no empty-list text ("No items. Drop assets here or click +"); slot lists show no count; no drag-to-reorder; remove has no confirm or undo hint at the widget level; add icon is far from the content in wide rows.
- Inconsistencies: IconButtons (tight padding) sit right next to the slot's ContentButtons that the slot's own comment says look better than IconButtons, so one row mixes two button chromes.

### 7.5 List header (ListHeader)
- Purpose: title + add icon over navigation lists (graph layers, bus layout, effect systems).
- Files: ListHeader.cppm, ListHeaderImpl.cpp. 22dp high; title 12px; add IconButton with caller tooltip.

### 7.6 List/Grid toggle (ViewModeToggles)
- Purpose: switch a view of assets between list and grid.
- Files: ViewModeToggles.cppm, ViewModeTogglesImpl.cpp. Two-segment SegmentedToggle with 16px viewList/viewGrid icons, tooltips "List"/"Grid", padding 4x3.
- Used by: Assets panel, Asset Picker, Project Manager.

### 7.7 Segmented toggle and tool panel kit
- See 4.7. SegmentedToggle: exclusive ToggleButtons, spacing 3, optional tooltips; re-lights silently.

### 7.8 Category tabs (CategoryTabs)
- Purpose: one tab per settings category, each a padded scrolling column, shared by Preferences, Project Settings and Export.
- Files: CategoryTabs.cppm. Tabs non-closable; AddNote() = 11px grey wrapped explanatory line.
- Problems: no tab icons, no search across tabs; notes are visually weak.

### 7.9 Shortcut capture button (ShortcutCaptureButton)
- Purpose: show and rebind a chord.
- Files: ShortcutCapture.cppm.
- Behaviour: shows "Ctrl+Shift+Z" or "(none)"; click (or Return/Space) starts capture with text "Press a chord... (Esc cancels, Del clears)"; Escape cancels, Backspace/Delete clears, modifiers alone wait, focus loss cancels.
- Problems: the capture prompt is longer than the 150dp button; Gui (Windows/Super) modifiers are captured by Normalize but FormatShortcut never prints "Gui+", so such chords display without it; no conflict indication while capturing.

### 7.10 Project picture (MakeProjectPicture)
- Purpose: a project's thumbnail or an initial-letter tile.
- Files: ProjectPicture.cppm. Used by the Project Manager rows/tiles and the Project page header.

### 7.11 Property name prettifier
- Files: PropertyNames.cppm. "castsShadows" -> "Casts Shadows"; drives row labels in inspectors and asset pages (inconsistent with hand-written labels like "Window resizable" in Project Settings, which are sentence case).

### 7.12 Tooltips
- Plain TooltipText on many controls (slot buttons, console chips, profiler buttons, palette none, menus none). One interactive tooltip: Project Manager grid tile (name, engine detail, path, [Open][Show in Folder][Copy Path]).
- Problems: page toolbar buttons, menu items and asset rows have none; asset rows could show path, type, cook state, size.

### 7.13 Drag and drop visuals
- Asset drag: default themed grey rounded ghost (Foundation/UI/DragDrop/DragAdorner.cppm) with no icon or label (AssetCell::CreateDragVisual returns empty).
- Drop feedback: 2px ring (accent match / error mismatch) on AssetPickerSlot and the ContainerListEditor append zone; dock zone indicators for panels (toolkit).
- Problems: no "not allowed" cursor or label; OS file drops have no hover feedback at all.

### 7.14 Icons (EditorIcons)
- Files: EditorIcons.cppm, EditorIconsImpl.cpp. Inline 24x24 SVGs baked to an atlas: translate, rotate, scale, worldSpace, localSpace, grid; asset types scene, prefab, mesh, skinnedMesh, material, texture (also ImageAsset), particleFx, animation, animGraph, skeleton, folder, unknown; viewList, viewGrid; close, search; add, remove (trash), moveUp, moveDown, copy, edit (pen); warning, more; brushRaise, brushLower, brushSmooth, brushFlatten. Template icons from Editor.Project/TemplateIcons.cppm.
- Problems: no icons for most asset types (see 4.3); the action registry's `icon` field is never drawn anywhere; no icons in menus or page toolbars.

---

## 8. Agent (MCP) surfaces that affect what the user sees
- Files: EditorActionTools*, EditorLayoutTools*, EditorPageTools*, EditorProfilerTools*, EditorWindowTools*, EditorMcpHost*, EditorProjectOperations*.
- `action_execute` runs actions unattended: any dialog an action opens is suppressed and closed as cancelled. `panel_dock` can rearrange panels; `page_open/close/reload` act on tabs; `editor_screenshot` captures windows. Each tool call writes "MCP: <tool> done/failed" to the status bar.
- Problems: there is no persistent indicator that an agent is connected or acting (only transient status text), and no log of agent actions in the UI beyond the Console.

---

## 9. Cross-cutting inconsistencies (ranked)
1. Commit models differ per dialog: Preferences and Project Settings stage until Save; Preferences' domain checkboxes write instantly (Cancel does not revert); Export auto-saves on selection change and close; preset Remove is instant while template Remove confirms.
2. Confirmation and button vocabulary: Dialog::Confirm "OK/Cancel" for deletes, ConfirmDialog verbs ("Remove/Cancel"), three hand-built "Unsaved changes" dialogs with different verbs ("Discard", "Exit Without Saving", "Close Without Saving"); "Close" vs "Cancel"; "&&" vs "&" in labels; no default button or Enter-to-confirm anywhere except the Command Palette.
3. Search fields: Console has a magnifier, a clear X and "Filter lines"; Assets "Filter all assets..." and Picker "Filter all groups..." have neither; PathPicker "Filter..."; Palette "Type an action's name..."; GroupPicker and AssetCreate have none. Only the Palette supports arrow keys and Enter from the field.
4. Validation feedback: Save As red "NOT saved: ..." on click; Asset Create grey lowercase live line with the button still enabled; New Project silent no-op; Import plan names unvalidated; inline rename rejects filesystem characters but Asset Create and Save As do not.
5. Favorites and cook state markers: browser gold corner dot vs picker "* " prefix and gold text; cook state as "[cooked]" text plus name colour in list mode, colour only in grid mode, nothing in the picker.
6. Group tree implemented four times (browser 22px/13px; picker, group picker, create 20px/12px), none with folder icons, preselection not highlighted in GroupPicker and AssetCreate.
7. Button chrome for row actions: ContentButton 14px glyphs in AssetPickerSlot; IconButton default size in ContainerListEditor and ListHeader; IconButton 14px in Export; text "Remove" in Preferences preview rows; text "Remove..." and "Reveal Folder" in Templates.
8. Icon semantics: trash = "Clear the console" and "Remove row/preset"; close X = close tab, clear search, clear reference; most asset types share the "unknown" glyph; declared action icons, chords and descriptions are not shown in menus or page toolbars.
9. Naming and casing: "New X" (File) vs "Create > X" (Assets); "Rename" vs "Rename Group"; "Always Export" vs "Always export contents"; "Pin favorite"; "Select asset" vs "Select destination group"; uppercase "START/PROJECT/SCENES" headings vs title case elsewhere; "AssiduousEngine" vs "Editor" (About); three different actions labelled "Play".
10. Layout metrics: label columns 110dp (Preferences, Settings, Export), 150dp (Project page), 170dp (Shortcuts), 52dp with colons (Import); some dialogs fixed size (Preferences, Settings, Templates, Export height), others min/max ranges; message and status placement varies (status bar, manager status label, profiler toolbar label, palette status line, dialog error lines, toasts).

Other recurring gaps: empty states are missing in the asset browser, pickers, path picker, profiler views and console; keyboard paths are missing in the manager, asset list (Enter), pickers (Enter, current value), dialogs (Enter); progress for long work (cook, export, native build, template install, model prepare) exists only as a status bar line with no cancel.
