// Draconic::EditorCore - :context partition.
//
// EditorContext: the central service object handed to every page/panel/plugin (Sedulous's
// EditorContext, Traktor's IEditor). Holds the open project, the registries, the open pages +
// active page, the global asset selection, and the status sink. Per-subsystem editor modules
// (draconic.<sys>.editor) register their factories here from RegisterEditor(EditorContext&);
// the statically-assembled editor executable calls those entry points (design doc §3.1).

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.editor.core:context;

import draconic.foundation;
import draconic.resource;
import draconic.settings;
import :importer;
import draconic.content;
import :command;
import :selection;
import :page;
import :job_service;
import :project;

using namespace draconic::foundation;

export namespace draconic::editor
{
    /// User-facing notification severity (the application maps these to UI toasts).
    enum class NoticeKind : u8
    {
        Info,
        Success,
        Warning,
        Error
    };

    class EditorContext
    {
    public:
        EditorContext() = default;
        EditorContext(const EditorContext&) = delete;
        EditorContext& operator=(const EditorContext&) = delete;

        // === Events ===

        /// Open-pages list or active page changed.
        Function<void()> OnPagesChanged;

        /// Request an incremental cook (wired by the application to its cook service). Pages
        /// call this after saving a builder-backed asset so the cooked product (and every
        /// live proxy bound to it) refreshes without a manual Build > Cook All.
        Function<void(bool /*rebuild*/)> OnCookRequested;
        void RequestCook(bool rebuild = false);
        /// Transient status-bar text.
        Function<void(StringView)> OnStatus;

        /// Transient user-facing notification (toast). Unwired = falls back to the status bar,
        /// so pages can Notify unconditionally.
        Function<void(NoticeKind, StringView)> OnNotice;
        void Notify(NoticeKind kind, StringView message);

        // === Project ===

        /// The app owns the project; the context borrows it (null = no project open).
        void SetProject(EditorProject* project);

        // The app's background-job runner (build lane + light lane). May be null in
        // headless/test contexts - callers must fall back to synchronous work.
        [[nodiscard]] EditorJobService* Jobs() const noexcept { return m_jobs; }
        void SetJobs(EditorJobService* jobs) noexcept { m_jobs = jobs; }
        [[nodiscard]] EditorProject* Project() const noexcept { return m_project; }

        // === Importers (OS file -> Sources/ + typed Asset instance; exe-registered) ===
        [[nodiscard]] ImporterRegistry& Importers() noexcept { return m_importers; }

        // === Resources (runtime products over the project's cooked DB) ===
        // Owned by the application (created at project open); pages resolve scene refs and the
        // inspector's pickers bind through it. Null until a project is open.
        void SetResources(draconic::resource::ResourceManager* resources) noexcept;

        /// The PER-PROJECT editor-state settings store (<project>/Editor/ - dock layout,
        /// favorites, open pages, per-page prefs). Borrowed; the app owns it and sets it for
        /// the lifetime of the open project (null between projects). Pages mutate their
        /// section + MarkChanged, then RequestProjectEditorSettingsSave() to persist.
        void SetProjectEditorSettings(draconic::settings::Settings* store) noexcept
        {
            m_projectEditorSettings = store;
        }
        [[nodiscard]] draconic::settings::Settings* ProjectEditorSettings() const noexcept
        {
            return m_projectEditorSettings;
        }
        Function<void()> OnProjectEditorSettingsSaveRequested; // app-bound persist hook
        void RequestProjectEditorSettingsSave()
        {
            if (OnProjectEditorSettingsSaveRequested)
            {
                OnProjectEditorSettingsSaveRequested();
            }
        }
        [[nodiscard]] draconic::resource::ResourceManager* Resources() const noexcept;

        // === Registries ===

        [[nodiscard]] EditorPageRegistry& Pages() noexcept { return m_pageRegistry; }

        /// Asset creators (File > New <label>): create a fresh source instance in the project DB.
        /// Registered by per-subsystem editor modules; the shell builds menu items from them.
        struct AssetCreator
        {
            String label;
            // Menu grouping: creators sharing a category land in a submenu of that name
            // ("Primitives"); empty = a top-level "New <label>" item.
            String category;
            // `group` = the browser group the user invoked the creator FROM (null = no context,
            // e.g. the File menu - the creator picks its own default group).
            Function<draconic::content::Instance*(EditorContext&, draconic::content::Group*)>
                create;
            // Only document-like creations (scenes) become the project's default scene when it
            // is unset; data assets (primitive meshes, materials) never should.
            bool setsDefaultScene = false;
        };

        // === Favorites (pinned asset instances - the browser + picker surface them first) ===

        [[nodiscard]] bool IsFavorite(const Guid& id) const;
        void ToggleFavorite(const Guid& id);
        [[nodiscard]] Span<const Guid> Favorites() const noexcept;
        void SetFavorites(Array<Guid> favorites) { m_favorites = Move(favorites); }
        /// Fired on every toggle (the app persists to the project's Editor/ state).
        Function<void()> OnFavoritesChanged;

        // === Editor clipboard (cross-page: entity subtrees, components) ===
        // One typed slot: `kind` says what the blob is ("entities", "component"); consumers
        // check the kind before parsing. Cleared by overwrite only.

        void SetClipboard(StringView kind, Array<byte> data);
        [[nodiscard]] StringView ClipboardKind() const noexcept { return m_clipboardKind.AsView(); }
        [[nodiscard]] Span<const byte> ClipboardData(StringView kind) const noexcept;

        void RegisterCreator(AssetCreator creator);

        [[nodiscard]] Span<const AssetCreator> Creators() const noexcept;

        // === Open pages ===

        /// Open (or focus) a page editing `instance`: an existing page for the same instance is
        /// activated; otherwise the registry's nearest-type factory creates one. Null if no
        /// factory matches or the instance's type isn't registered.
        EditorPage* OpenPage(draconic::content::Instance& instance);

        /// Adopt an instance-LESS page (the Game tab): same ownership + active-page flow as
        /// OpenPage, but the caller constructs it (no instance, no factory dispatch).
        EditorPage* AdoptPage(UniquePtr<EditorPage> page);

        /// Close a page (the caller is responsible for save-prompting dirty pages first).
        void ClosePage(EditorPage* page);

        [[nodiscard]] Span<const UniquePtr<EditorPage>> OpenPages() const noexcept;

        [[nodiscard]] EditorPage* ActivePage() const noexcept { return m_activePage; }
        void SetActivePage(EditorPage* page);

        // === Edit routing (menu Edit>Undo/Redo -> the active page's stack) ===

        [[nodiscard]] bool CanUndo() const;
        [[nodiscard]] bool CanRedo() const;
        void Undo();
        void Redo();

        // === Selection ===

        /// Global asset selection (asset browser / instance pickers). Entity selection is
        /// per-scene-page (phase 3).
        [[nodiscard]] Selection<const draconic::content::Instance*>& AssetSelection() noexcept;

        // === Import notifications ===

        /// Subscribe to successful file imports (fired by the import flow AFTER the importer
        /// returned; `options` is the dialog-edited options object, null when none). Used for
        /// post-import steps that live above the importer's layer - e.g. model->prefab
        /// generation, which needs scene machinery the importer library never links.
        void AddImportListener(
            Function<void(draconic::content::Instance&, const ImportOptions*)> listener);

        /// Play-in-editor seam: creates the singleton Game page (the player behavior in a
        /// tab). Registered by the scene editor plugin; unset = the Game menu item notifies.
        // Creates a Game tab. `newInstance` = false reuses the app's primary GameInstance (the normal
        // Play); true spins up an ADDITIONAL instance (multi-instance PIE, game-instance.md §11 step 5).
        Function<UniquePtr<EditorPage>(bool newInstance)> GamePageFactory;
        /// Stops the Game tab's live run, if any (the embedded app's RequestExit lands
        /// here, deferred to after the page-update loop). Set by the Game page.
        Function<void()> StopGameRun;

        /// Export seam: transcodes a scene/prefab instance's TEXT source stream to the
        /// binary wire for staging. Registered by the scene editor plugin (needs scene
        /// machinery editor.core never links); returns false for non-scene instances or
        /// on failure (the exporter then stages the source verbatim - the runtime sniffs).
        /// MAIN-THREAD only (creates a scratch scene through the SceneSubsystem).
        Function<bool(draconic::content::Instance&, Array<byte>&)> SceneStreamStager;

        /// Export reachability seam: collects the assets a scene/prefab instance references, for the
        /// export closure (docs/design/export-reachability.md). Loads the instance over the app's full
        /// manager set, appending its component resource Ref ids to `outResources` and its nested
        /// prefab-instance ids to `outPrefabs`; returns false for non-scene instances or on failure.
        /// Registered by the scene editor plugin (needs scene machinery). MAIN-THREAD only (loads a
        /// scene through the SceneSubsystem), which is exactly why the editor pre-scans with this on
        /// the main thread and hands the resulting guid set to the background export job.
        Function<bool(draconic::content::Instance&, draconic::content::ContentDatabase&,
                      Array<Guid>& /*outResources*/, Array<Guid>& /*outPrefabs*/)>
            SceneRefScanner;

        void NotifyImported(draconic::content::Instance& instance, const ImportOptions* options);

        // === Script breakpoints (the debugger, script-debugger.md P1) ===
        // Shared editor state: the ScriptPage gutter toggles them per source file+line, and a
        // Game run applies them to its script debugger. Contract-neutral plain data (a future
        // remote debugger consumes the same set).

        struct ScriptBreakpoint
        {
            String file;  // source file name (the section the runtime reports)
            i32 line = 0; // 1-based
        };

        /// Toggle a breakpoint at `file:line`; fires OnBreakpointsChanged.
        void ToggleBreakpoint(StringView file, i32 line);
        [[nodiscard]] bool HasBreakpoint(StringView file, i32 line) const;
        [[nodiscard]] Span<const ScriptBreakpoint> Breakpoints() const noexcept;
        /// Fired on every breakpoint toggle (the gutter repaints; a live run re-applies).
        Function<void()> OnBreakpointsChanged;

        // === Script execution point (paused debugger location) ===
        // Set by a Game run's debugger listener on Breakpoint/Stepped (the innermost stack
        // frame), cleared on resume/stop. The ScriptPage editing that file shows it as the
        // ExecutionLine marker. Version-stamped so consumers can poll cheaply per frame.

        struct ScriptExecutionPoint
        {
            String file;    // source file name (as the runtime reports it)
            i32 line = 0;   // 1-based
            bool active = false;
        };

        void SetScriptExecutionPoint(StringView file, i32 line)
        {
            m_executionPoint.file = String(file);
            m_executionPoint.line = line;
            m_executionPoint.active = true;
            ++m_executionPointVersion;
        }
        void ClearScriptExecutionPoint()
        {
            if (!m_executionPoint.active)
            {
                return;
            }
            m_executionPoint.active = false;
            ++m_executionPointVersion;
        }
        [[nodiscard]] const ScriptExecutionPoint& ScriptExecution() const noexcept
        {
            return m_executionPoint;
        }
        [[nodiscard]] u64 ScriptExecutionVersion() const noexcept
        {
            return m_executionPointVersion;
        }

        /// Debugger value lookup for hover inspection: given an identifier, returns its
        /// display text ("value : Type"), or empty when unavailable (no run, not paused,
        /// unknown name). Installed by the Game page for the run's lifetime; the probe
        /// itself checks pause state. Contract-neutral plain data, like the breakpoint
        /// store - a future remote debugger installs the same shape.
        Function<String(StringView)> ScriptValueProbe;

        // === Status ===

        void SetStatus(StringView text);

    private:
        void NotifyPagesChanged();

        EditorProject* m_project = nullptr;
        EditorJobService* m_jobs = nullptr; // borrowed (app-owned)
        draconic::resource::ResourceManager* m_resources = nullptr; // borrowed (app-owned)
        draconic::settings::Settings* m_projectEditorSettings = nullptr; // borrowed (app-owned)
        ImporterRegistry m_importers;                               // borrowed
        EditorPageRegistry m_pageRegistry;
        Array<AssetCreator> m_creators;
        String m_clipboardKind;
        Array<byte> m_clipboard;
        Array<Guid> m_favorites;
        Array<Function<void(draconic::content::Instance&, const ImportOptions*)>> m_importListeners;
        Array<UniquePtr<EditorPage>> m_pages;
        EditorPage* m_activePage = nullptr;
        Selection<const draconic::content::Instance*> m_assetSelection;
        Array<ScriptBreakpoint> m_breakpoints; // shared script debugger breakpoints
        ScriptExecutionPoint m_executionPoint; // paused-debugger location (versioned)
        u64 m_executionPointVersion = 0;
    };
}
