# Editor actions - one declaration behind menus, shortcuts, toolbars, the palette and MCP

**Status:** BUILT through layer 6 (2026-09-27); layer 7 is PROPOSED below (2026-09-27). Originally PROPOSED 2026-09-26. Asked for by the user on
2026-09-25 ("something I have been observing for a while") when the MCP action bridge showed
there is no action system to bridge.

## The problem as it stands

The editor binds every user command at the surface that shows it:

- Menus: `file->AddItem(u8"Save", [this]() { SaveActivePage(); });` - 99 `AddItem` sites in
  11 files (ApplicationImpl 25, AssetsView 26, HierarchyView 17, the page impls the rest).
- Shortcuts: `shortcuts->AddGlobal(KeyCode::S, KeyModifiers::Ctrl, [this]() { SaveActivePage(); });`
  - 4 globals in ApplicationImpl, a second copy of the menu's lambda each time.
- Toolbars: `m_save->OnClick.Add([self](ToolbarButton*) { (void)self->m_page->Save(); });`
  with `PageToolbar::Refresh()` syncing enabled states per frame - 128 `OnClick.Add` sites in
  36 files (many are dialog buttons and rows, not commands).
- MCP: nothing can list or run "what the user can do", so `action_list / action_state /
  action_execute` (the spec's layer 7), the unattended scope and the Agent panel all wait.

Consequences: a command exists as many times as it has surfaces, its label and its enabled
rule are repeated or missing (menus are always enabled; only PageToolbar syncs), shortcuts
cannot be rebound or listed, there is no command palette, and no other client (an agent, a
test, a script) can drive the editor by command.

## Prior art (read 2026-09-26; ~/Dev/CPP/LumixEngine, ~/Dev/CPP/ZeroCore)

**Lumix** (`src/editor/action.h`, `studio_app.cpp`, `settings.cpp`). `Action` is a value
declared by its OWNER as a member of the panel that implements it
(`Action m_back_action{"Asset browser", "Back", "Back in history", "asset_browser_back", ICON}`),
self-linked into a global intrusive list at construction. It carries identity (`name`, used
for shortcut persistence), `group`, short and long labels, an icon, the shortcut chord, and a
`type` (NORMAL / TOOL / WINDOW / TEMPORARY). It carries NO behaviour and NO enabled rule:
menus, the toolbar and the command palette set `action.request = true`, and the owner asks
`checkShortcut(action)` in its own update, which answers true on a request OR on the chord -
so the owner runs the action in its own context. Enabled is a parameter of the menu builder
(`menuItem("undo", m_editor->canUndo())`). Menus are still hand-written lists of names; the
palette and the shortcut editor (collision detection, persisted by name) fall out of the list.
Fits immediate-mode ImGui, where the owner polls every frame anyway.

**Zero** (`Code/Extensions/Widget/Command.hpp`, `CommandBinding.hpp`, `Data/Commands.data`,
`Menus.data`, `Toolbars.data`, `Shortcuts.data`). `Command` is a DATA record loaded from
`Commands.data` (Name, Description, Shortcut, IconName, Tags, ReadOnly, DevOnly); code binds
behaviour by name in one place (`commands->AddCommand("Undo", BindCommandFunction(EditorUndo))`).
Behaviour is a `CommandExecuter` with `Execute` AND `IsEnabled`, both given a `Context`: a
typed handle map the focused widget fills by bubbling a `CommandCaptureContext` event
(Editor, Space, Viewport, Project); `Command1<T>` binds a function taking a `T*` and is enabled
iff the context holds a `T`. Menus and toolbars are data too (`Menus.data`: named lists of
command names plus `Divider`), built by looking each name up; buttons and menu items reflect
enabled and active state through `CommandStateChange` events (retained widgets). The chord
table maps a shortcut string to a command; user overrides persist in `Shortcuts.data`; a
search provider gives the command palette; `ReadOnly` gates execution in read-only mode;
`CogCommand` turns script components into commands.

**What each gets right for us.** Lumix: the owner declares its actions where they live, and
the palette and shortcut editor come free from the one list. Zero: behaviour and enabled rule
are ONE binding evaluated against a typed CONTEXT, and every surface is built FROM the
command set rather than binding on its own. What neither is: Lumix's self-registering global
list and Zero's singleton + data-file binding by string are both against our explicit
composition root, and Zero's event plumbing exists for retained widgets that never poll -
ours already refresh per frame.

**Traktor** (read 2026-09-26; `code/Ui/Command.h`, `code/Editor/App/EditorForm.cpp`,
`IEditorPage.h`, `IEditorPlugin.h`, `resources/runtime/configurations/Traktor.Editor.config`).
`ui::Command` is a VALUE: an id and/or a dotted name (`Editor.Save`, `Scene.Editor.AddEntity`)
plus an optional data object (the recent-workspace path, the instance to open). It carries
no label, no behaviour, no enabled rule and no chord. Menus and toolbars are built by hand
with `MenuItem(Command, i18n label)` / `ToolBarButton(..., Command)`; a click raises an event
carrying the command, a chord in the accelerator table raises the same, and every event path
funnels into ONE `EditorForm::handleCommand(command)`: the form's own `if (command ==
L"Editor.Save")` ladder, then the database view or a modal object editor, then the active
page's `handleCommand` (its own ladder, which first offers the command to its focused
sub-view - the properties view takes Delete/Copy before the scene does), then every plugin,
which sees the command and the result so far. The set of shortcut-able commands is GATHERED
at startup from each contributor (`IEditorPageFactory::getCommands`, `IEditorPlugin::
getCommands`, `IObjectEditorFactory::getCommands`, plus the form's own list); each chord
comes from the `Editor.Shortcuts` settings group (defaults in the config, a Shortcuts
settings page with collision detection). Enabled state does not exist: menus show everything,
a handler no-ops when nothing applies, and `result` says only whether someone took it. The
MCP plugin does not touch commands (empty `getCommands`, `handleCommand` returns false).

**What Traktor confirms and adds.** Confirms: one funnel (a menu click, a chord, a toolbar
click and a future `action_execute` are the same value at the same dispatch point - ours is
`Registry::Execute(id)`), and declarations gathered from each contributor in an explicit
composition root rather than discovered. Adds two rules to ours: (1) an action is NULLARY -
Traktor's data-carrying commands ("open recent X") are list-driven menus, which stay
list-driven here (the creators submenu, recent projects) and are not actions; anything that
takes a parameter is a tool, not an action. (2) FOCUS FIRST for the keys a focused view owns:
Traktor hands Delete/Copy to the properties view before the page; we already dispatch
shortcuts after the focused view, with text controls marking their key-downs handled, and an
action's `execute` binds to the active page's selection, so a focused sub-view that wants the
key keeps it by handling it, exactly as today. What Traktor lacks and we keep: labels and
descriptions on the declaration (Traktor's live at the menu, so a command can be in the
shortcut list and not in a menu, or the reverse), an enabled rule the surfaces and the agent
can read, and a registry that knows what a name means instead of a string ladder per handler.

**ezEngine** (read 2026-09-25 for MCP): the action bridge lists, reports and executes the
editor's registered actions with declared unattended answers for the dialogs an action may
open; the shape our `action_*` tools take once there is a registry.

## What already fits in our editor

- `EditorContext` already routes commands with an enabled rule: `CanUndo()/Undo()` to the
  active page's stack; `PageToolbar::Refresh()` syncs enabled per frame from `IsDirty` and
  `CanUndo/CanRedo`.
- The File > New submenu is BUILT from a registry (`EditorContext::Creators()`, grouped by
  category) - the one place a menu already follows a declaration list.
- A page publishes the interfaces it implements (`IPageService`, `EditorPage::Provide /
  Service<T>`): this IS Zero's typed context for page-level commands - "enabled iff the active
  page provides `ISceneEditorPage`" needs no capture event.
- `ContextMenu::AddItem(label, action, enabled)` already carries enabled per item, evaluated
  when the menu is built (menus build on open).
- `ShortcutManager` has global and view-scoped chords; `EditorSettings` sections persist
  preferences; `EditorIcons` names the icon slots; `RegisterSceneEditor` and its peers are the
  composition root where each domain registers what it contributes (pages, creators, MCP tool
  contributions).

## Design

One declaration per command, registered in the composition root, and every surface built
from the registry. Editor.Core, partition `editor.core:actions`.

```
struct EditorShortcut { ui::KeyCode key; ui::KeyModifiers modifiers; };   // none = no chord

enum class ActionKind : u8 { Command, Toggle, Window };   // Toggle/Window carry a checked state

struct EditorActionDeclaration
{
    StringView id;            // stable, dotted, the serialization + MCP + palette identity:
                              // "file.save", "edit.undo", "scene.simulate.start"
    StringView label;         // "Save"                (menus, toolbars)
    StringView description;   // "Save the active page" (palette, tooltips, MCP)
    StringView menuPath;      // "File/Save", "Edit/Undo", "Scene/Simulate/Start" - the menu
                              // bar is GENERATED from these; empty = not in the menu bar
    i32 menuOrder;            // within its menu; separators between order bands (100s)
    EditorIconSlot icon;      // None = text only
    EditorShortcut shortcut;  // the default chord; the user's override wins
    ActionKind kind;
    bool readOnly;            // changes nothing (MCP readOnlyHint; runs under a read-only editor)
    Function<void()> execute;
    Function<bool()> enabled; // unset = always
    Function<bool()> checked; // Toggle / Window only
};
```

- **Registry.** `EditorActionRegistry` owned by `EditorContext` (`context.Actions()`), beside
  `Pages()`, `Creators()` and the MCP contributions: `Register(declaration)` (an id registered
  twice is a programming error, asserted), `Find(id)`, `ForEach`, `IsEnabled(id)`,
  `IsChecked(id)`, `Execute(id) -> Status` (NotFound for an unknown id, NotSupported when not
  enabled - the surfaces never call a disabled action, MCP reports it), `Shortcut(id)` (the
  effective chord), `Rebind(id, chord)` with collision detection, `OnActionsChanged` for the
  surfaces that cache (the menu bar rebuilds, the shortcut table rebinds).
- **Context = the active page's services.** A page-level action binds through the context:
  `enabled = [ctx]{ auto* p = ctx->ActivePage(); return p != nullptr && p->IsDirty(); }`,
  `execute = [ctx]{ (void)ctx->ActivePage()->Save(); }`. A domain action binds through the
  service it needs: `ctx->ActivePage()->Service<ISceneEditorPage>()` - null means disabled.
  A helper keeps that one-liner honest: `RequiresService<ISceneEditorPage>(ctx)` wraps
  `enabled` and hands `execute` the service. Selection-driven actions (rename, duplicate,
  create prefab from selection) read the page's edit context the same way - the selection is
  the context, exactly as for the MCP tools. Nothing captures context by event.
- **Who registers.** The application registers the editor-wide set (file, edit, view, game,
  help) in its composition root; each domain's `Register<Domain>Editor` registers its own
  (the scene editor: simulate start/stop/pause, gizmo modes as Toggles, the markers Window,
  the hierarchy's entity actions), beside its pages, creators and MCP contribution. Explicit,
  listed once, no self-registration, no discovery.
- **Surfaces built from the registry.**
  - Menu bar: generated from `menuPath` + `menuOrder`; each item is `AddItem(label,
    [id]{ actions.Execute(id); }, actions.IsEnabled(id))` with the effective shortcut in the
    label column; Toggle/Window items show `checked`. The bar rebuilds on `OnActionsChanged`
    (project open registers more) and menus evaluate `enabled` when they open. The creators
    submenu stays a creators submenu (it is a registry of its own).
  - Shortcuts: the registry binds every action with a chord on the `ShortcutManager` as a
    global shortcut whose callback is `Execute(id)`; a rebind re-registers. Page-scoped
    chords (a view must be focused) are the exception and stay on the view, declared by the
    same action with a `scope` in a later step if needed.
  - Toolbars: `PageToolbar` becomes a toolbar of action ids (`"page.save"`, `"page.undo"`,
    `"page.redo"`, `"page.discard"`); `Refresh()` syncs `IsEnabled` / `IsChecked` per frame
    as it does today; a domain adds its ids (`AddAction("scene.simulate.start")`).
  - Command palette (Ctrl+Shift+P): the registry filtered by label / description / id,
    Enter executes - free once the registry exists; the user gets it early.
  - Preferences > Shortcuts: the registry with `Rebind` and collision reporting; overrides
    persist in an `EditorSettings` section keyed by id.
  - MCP: `action_list` (id, label, description, kind, readOnly, enabled, checked, shortcut),
    `action_state(id)`, `action_execute(id)` - refused when disabled, `readOnly` mapped to
    the tool hint; `action_execute` runs under the unattended scope and reports the dialogs
    it suppressed (ezEngine's contract). One generic bridge, the whole surface.
- **A subject page.** An action is nullary to every surface and to the agent, but it runs
  OVER a page the surface supplies: the active page for the menu bar, a chord, the palette
  and the MCP bridge (the registry's `ActiveSubject`, wired by the context), and a page's
  OWN page for that page's toolbar - a split layout shows two pages and only one is active,
  and the toolbar inside the other must act on and report about its own page. So the three
  bindings take `EditorPage*` (`enabled`, `checked`, `execute`; an editor-wide action ignores
  it), `ServiceOf<T>(page)` is how a domain action reaches the interface the subject
  publishes, and the registry answers `IsEnabled(id)` / `Execute(id)` over the active subject
  or `IsEnabled(id, page)` / `Execute(id, page)` over a named one. One rule, whichever surface
  asks (found while building the toolbar layer).
- **Not in the declaration.** Enabled/checked are PULLED (menus on open, toolbars per frame,
  MCP on call) - no state-change events. Execution is a direct call on the main thread -
  no request flag (Lumix's exists for polling owners). Undo stays where it is: a mutating
  action goes through the page's command stack like the menu lambda it replaces.

## Layer 7 - the remaining surfaces (PROPOSED 2026-09-27)

Layers 1 to 6 moved the application's menus, the scene editor's hierarchy menus and toolbar,
the page toolbar, the palette and the Shortcuts page onto the registry. What still binds a
command at the surface that shows it, counted 2026-09-27 (`AddItem(u8"` sites outside tests):

| File | Items | What they are |
|---|---|---|
| Editor.App/AssetsViewImpl.cpp | 20 | the asset browser's row, group and background menus |
| Editor.Scene/HierarchyViewImpl.cpp | 2 | Rename, Copy ID over an entity |
| Editor.Particles/ParticleEffectPageImpl.cpp | 2 | Move Down, Delete over an emitter |
| Editor.Animation/AnimationGraphPageImpl.cpp | 2 | Make Transition, Delete Transition over a node |
| Editor.App/ApplicationImpl.cpp | 1 | a combo box entry, not a command |

The asset browser is the layer. Its three menus today:

- **Row over an instance:** Open | Rename, Duplicate | Copy GUID, Copy Path, Pin / Unpin
  favorite, Add to / Remove from Always Export | Cook, Rebuild | Delete (multi-select aware:
  every selected instance, the label counting them).
- **Row over a group:** Open | Rename | Cook Group, Rebuild Group | Delete Group.
- **Background:** Create > (the creators, by category) , New Group, Import... | (inside a
  group: Rename Group, Cook Group, Rebuild Group, Delete Group) | Cook All, Rebuild All.

### The subject: the asset selection, published by the browser

An action is nullary to its surfaces and runs over a subject the surface supplies. For the
asset browser the subject is not a page: it is the context's asset selection
(`EditorContext::AssetSelection()`, `Selection<const content::Instance*>`), which exists and
which nothing publishes today - the browser keeps its own `SelectionModel` over rows
(`SelectedInstanceIds`). So:

- The browser PUBLISHES its selection into `AssetSelection()` on every change (the hierarchy
  publishes into the page's `EntitySelection()` the same way). Multi-select stays: the
  selection's items are the targets, the primary is the clicked row.
- The asset actions bind through the context, ignoring the page subject: `enabled` is
  "the asset selection is non-empty" (or "exactly one" for Rename, Duplicate, Copy GUID,
  Copy Path, Open), `execute` acts on `AssetSelection().Items()`. Nothing widens the
  registry's subject notion; an editor-wide action ignoring its page subject is already the
  rule for File and Project.
- Ids under `asset.`: `asset.open`, `asset.rename`, `asset.duplicate`, `asset.copyGuid`,
  `asset.copyPath`, `asset.toggleFavorite` (a Toggle: checked = the primary is a favorite),
  `asset.toggleAlwaysExport` (a Toggle), `asset.cook`, `asset.rebuild`, `asset.delete`.
  Labels as today; the counting Delete label becomes the description ("Delete the selected
  assets") with the count in the confirmation dialog, since a declaration's label is fixed.
- `asset.open` goes through `EditorContext::OpenAsset` (the seam the browser's
  `OnOpenInstance` already reaches, interceptors included), so the palette opens an asset
  the way a double click does. `asset.rename` starts the browser's inline edit: it needs the
  browser, so the browser REGISTERS that one itself with a `Function` it owns (as the scene
  editor registers its actions with the page's services), and it is disabled when the
  browser is not showing the primary.
- Cook and Rebuild over the selection use the cook service the browser already calls
  (`RequestCookFor(roots, rebuild)`); favorites and export roots use the context's
  `IsFavorite` / `ToggleFavorite` and the export-root set the browser edits today.

### What stays on the browser

- **Groups.** A group is a folder the browser is showing, not an asset in the selection.
  Rename Group is an inline edit, New Group / Delete Group / Cook Group / Rebuild Group act
  on the browsed group. They stay hand-bound on the browser: they are navigation of the
  browser's own tree, not commands a palette or an agent asks for by name (an agent has
  `asset_list` / `asset_cook` with a group filter). If a later need shows, the browsed
  group becomes a second published selection and they join the same way.
- **Create >** is the creators registry (`EditorContext::Creators()`), already a registry of
  its own and already the File > New submenu's leading items; the browser keeps building it
  with the same helper.
- **Import..., Cook All, Rebuild All** are editor-wide: `project.import` (the import dialog),
  `project.cook`, `project.rebuild` join the application's declarations under Project, and
  the background menu appends them by id. Cook All / Rebuild All then also reach the palette
  and a chord, which they never could.

### The page leftovers

Same rule as the scene editor's actions: a page-owned command binds through the service the
page publishes, over the page's own selection.

- Hierarchy: `scene.entity.copyId` joins SceneActionIds (enabled: a primary exists); Rename
  stays hand-bound - it starts the hierarchy's inline edit, the same reason as
  `asset.rename`, and the hierarchy registers it itself if the palette ever wants it.
- Particle page: `particle.emitter.moveUp` / `moveDown` / `delete` over the page's emitter
  selection through `IParticleEditorPage` (published by the page, `Provide<>` as the scene
  page does); the emitter list's menu appends them by id.
- Animation graph: `animGraph.node.makeTransition` / `deleteTransition` through
  `IAnimationGraphPage` over the canvas selection; the node menu appends them by id.

### The page toolbar roll-out (the carried backlog item, from week 2026-08-29)

The PageToolbar is a registry toolbar over ITS page since layer 3. The pages that still
have none get it with the same recipe (wrap the content root in a column with the toolbar
on top, `Refresh()` in `OnUpdate`): material, animation clip, animation graph, particle
effect, font, heightfield, terrain, input map, the generic asset form. The gotcha from the
first batch holds: a page that mutates its asset directly (no commands) overrides
`DiscardChanges` to reload from the source DB, or Discard clears dirty without reverting.
A domain adds its own ids to its toolbar with `AddAction`.

### Tests

- Editor.App.Tests: the browser publishes its selection into the context (single, multi,
  clear); each asset action's `enabled` over an empty, single and multi selection; `execute`
  of copyGuid / copyPath (clipboard text), toggleFavorite (checked follows), cook (the cook
  service seam sees the roots), delete (the confirmation seam sees the targets); the row
  menu built from ids matches the declarations in order; the background menu's
  editor-wide ids resolve.
- Editor.Scene.Tests: copyId over the headless page; the particle and graph page interfaces
  on a headless fixture with their actions' enabled and execute.
- The MCP action bridge needs no new test: `action_list` lists the new ids as it lists any.

### Not in this layer

- A second published selection for groups (see above).
- Drag-and-drop, double click and inline rename gestures: those are the browser's, not
  commands.
- The asset browser's own toolbar (filter, view mode): view state, not actions.

## Landing order (each its own commit with tests, both compilers, then the lanes)

1. `editor.core:actions`: the declaration, the registry on EditorContext, Execute /
   IsEnabled / IsChecked / Shortcut / Rebind with collisions; tests over a context with a
   page that provides a service.
2. The application's editor-wide actions registered in its composition root, and the menu
   bar plus the global shortcuts GENERATED from the registry - ApplicationImpl's 25 items and
   4 shortcuts become declarations; a test proves the generated menu matches the declarations.
3. `PageToolbar` as action ids over ITS page; the standard page set (file.save / edit.undo /
   edit.redo / page.discardChanges) as declarations over the subject page.
4. The scene editor's actions in `RegisterSceneEditor` (simulate, gizmo toggles, markers,
   hierarchy entity actions over the selection); the hierarchy context menu built from them.
5. The MCP bridge (`action_list / action_state / action_execute`) and the unattended scope.
   BUILT 2026-09-26: the scope is the UI context's `DialogInterceptor` (Foundation), installed
   by `UnattendedDialogs` for the call's duration - `Dialog::Show` is the one place every
   dialog appears, so a suppressed dialog closes as cancelled at once and the flow that asked
   proceeds as dismissed; the result names the titles. Layers 1 to 4 BUILT 2026-09-26 too.
6. The command palette and Preferences > Shortcuts with persistence.
   BUILT 2026-09-27 (branch editor-actions-palette): EditorShortcutSettings persists the
   overrides by id (captured from the registry, applied after every domain registered, an
   id this build never registered kept for a later run); CommandPaletteDialog is the
   registry filtered (label, description, id; ASCII folded; label-starts first), Enter runs
   over the active subject, Ctrl+Shift+P as view.commandPalette; Preferences > Shortcuts
   lists every action with a ShortcutCaptureButton (the next key pressed, modifiers
   normalised) and Reset, staged in ShortcutEdits and applied together on Save - swaps
   allowed, a collision keeps the holder's chord and is reported by name.
7. The remaining surfaces (the section above), in this order, each its own commit:
   a. The browser publishes its selection into `EditorContext::AssetSelection()`; tests.
   b. `project.import` / `project.cook` / `project.rebuild` as application declarations; the
      background menu appends them by id.
   c. The `asset.*` declarations over the asset selection (the browser registers
      `asset.rename` itself) and the row menu built from them; tests over the seams.
   d. The page leftovers: `scene.entity.copyId`, the particle emitter and animation graph
      actions through their pages' published interfaces; tests on headless fixtures.
   e. The page toolbar roll-out, page by page (one commit per page or per batch of alike
      pages), with the DiscardChanges rule where a page mutates its asset directly.
