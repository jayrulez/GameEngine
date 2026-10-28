# Projects, settings, and the built-in project manager

> Status: CURRENT
> Verified: 2026-08-12 @ 1abdb98d
> Track: [[project-manager-track]] / [[settings-and-export-track]]

The applied settings model + the Godot-style single-exe project manager. The settings PRIMITIVE
(`foundation.settings::Settings`) is `Documentation/Systems/settings.md`; this is how it is layered
into three concrete stores and how projects open/close.

## The three stores

| Store | File | Owner module | Holds |
|---|---|---|---|
| Shared project manifest | `<project>/Project.xml` | `engine.project` | name, engineVersion stamp, default scene/script/input-map/bus-layout refs, the GUI's sheets and loading screen. The SAME payload ships as `player.xml` in a dist (minus editor concerns). Versioned (`ProjectSettings`, data v10; a v9 manifest still reads, its old UI theme and loading document dropped); every save re-stamps `engineVersion` to the running engine. |
| Per-user editor settings | `<user-data>/draconic/editor.settings.xml` (`~/.local/share` on Linux, `%LOCALAPPDATA%` on Windows) | `editor.core` | machine-wide state: `EditorExportSettings` (templates root), `RecentProjectsSettings` (the project-manager registry). |
| Per-project editor state | `<project>/Editor/editor.project.settings.xml` | `editor.app` (+ contributor modules) | `EditorDockLayoutSettings`, `EditorFavoritesSettings`, `EditorOpenPagesSettings`, `MaterialPreviewSettings` (from the material page), ... |

All three persist through the same machinery: `foundation.settings::Settings` (typed `ISerializable`
sections, versioned envelopes) over `XmlSerializerFactory`. The engine/player side reads ONLY the first
store (`engine.project` is editor-free); the two settings stores are editor concerns. Layering is by
SEPARATE store instances (settings.md), not an in-store layer stack.

Rules:
- Sections must be REGISTERED (type registry + serializable registry) before a store loads - an
  unregistered section type fails the whole `Settings::Load`. Per-module sections register in that
  module's `RegisterXxxEditor` entry point, which runs before the app loads the per-project store.
- The registry lives OUTSIDE any engine install or project on purpose: every engine version on a machine
  lists the same projects, and a future separate launcher/hub reads the same file. One of the four
  hub-extraction seams (below).

## The project manager (single-exe, Godot-style)

- `Tools.Editor` with NO project argument starts on the manager screen (`ProjectManagerView`): recent
  projects (live-probed rows - missing dirs dim, newer-engine stamps amber), Open / Open Folder... /
  New Project... / Remove From List.
- `Tools.Editor <dir>` or `--project <dir>` opens directly: the single-project lifecycle (no Close
  Project menu item; the CLI path keeps the scaffold-on-open fallback).
- Decisions live in `ProjectManagerController` (`editor.project`/`editor.app`, headless, tested):
  `DecideOpen` probes the manifest and gates it - NotAProject / OpenDirectly / PromptOlderBackup /
  PromptNewerEngine - and composes the prompt copy. The App layer only renders dialogs. Project
  TEMPLATES will plug into `Create` here.
- Version prompts: older/unstamped projects offer Back Up & Open (copies `Project.xml` to
  `Project.xml.<oldver>.bak`, opens, immediately re-stamps via `SaveSettings`); newer-engine projects
  get a hard warning. Content assets migrate through their own versioned serializers as they re-save -
  whole-project safety is version control's job (the prompt says so).
- File > Close Project (manager-launched only) saves layout/pages, closes every page through the dirty
  prompt, shuts the cook service, detaches per-project resources from the embedded runtime
  (`DefaultApplication::AttachResourceManager(nullptr, ...)` - its `Resources()` consumers are lazy and
  null-tolerant), releases the project, returns to the manager. Teardown is queued through the UI
  mutation queue, never mid-event-dispatch.
- Every successful open (manager or CLI) touches the registry, so CLI-opened projects appear in the
  manager next launch.

## Future-hub extraction seams (kept deliberately)

1. Engine-version stamp in `Project.xml`, rewritten on every save.
2. The registry in the per-user store, independent of engine installs.
3. `--project <dir>` direct-open CLI - a hub is just "pick a version, exec with the flag".
4. Versioned manifest + explicit backup-then-migrate at open.

## Editor app lifecycle

App-lifetime (once per run): fonts, user settings, icons, UIHost/dock host, theme, shell chrome,
embedded runtime (starts WITHOUT resources), registerEditors, menus. Per-project
(`OpenProjectAt`/`CloseProject`): manifest + content DBs (`EditorProject`), per-project settings store,
favorites, ResourceManager (late-attached to the embedded runtime, which registers its standard
factories into it), project UI theme, cook service + assets view, saved pages + dock layout, registry
touch. The manager screen and the editor shell are two `RootView`s swapped on the same window via
`UIHost::DetachWindow`/`AttachWindow`.
