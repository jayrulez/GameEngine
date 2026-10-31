// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Core - :context partition.
//
// EditorContext: the central service object handed to every page, panel and plugin. It holds the
// open project, the registries, the open pages and the active page, the global asset selection,
// and the status sink. Per-subsystem editor modules register their factories here from
// RegisterEditor(EditorContext&), and the statically assembled editor executable calls those entry
// points.

module;
#include "Core/Prelude.h"

export module editor.core:context;

import foundation.core;
import foundation.resource;
import foundation.settings;
import foundation.mcp;
import pipeline.importer;
import pipeline.core; // AssetCreatorRegistry (File > New)
import foundation.content;
import foundation.gui; // IClipboard (CopyText)
import :command;
import :selection;
import :page;
import :actions;
import :thumbnail_service;
import editor.project; // EditorProject, the job service, the root allocator

using namespace foundation::core;

export namespace editor
{

    /// User-facing notification severity (the application maps these to UI toasts).
    enum class NoticeKind : u8
    {
        Info,
        Success,
        Warning,
        Error
    };

    /// A sink for closures that persist live asset edits (terrain sculpting, for one).
    /// EditorContext implements it; the viewport tool framework (editor.viewporttools) holds a
    /// borrowed IAssetEditSink* so a domain tool can register a closure that writes back to the
    /// source without the framework depending on EditorContext. The sink outlives every page and
    /// tool.
    class IAssetEditSink
    {
    public:
        virtual ~IAssetEditSink() = default;
        virtual void
        RegisterAssetEdit(const Guid& assetId,
                          Function<Status(foundation::content::ContentDatabase&)> persist) = 0;
    };

    class EditorContext : public IAssetEditSink
    {
    public:
        // The allocator (required; the editor application passes its tagged "Editor" root) backs
        // every page, panel and editor service.
        explicit EditorContext(IAllocator& allocator) noexcept
            : m_allocator(&allocator), m_importers(allocator), m_creators(allocator)
        {
            // The argument-less action calls (menus, chords, the palette, the MCP bridge) run over
            // the active page; a page's own toolbar names its page instead.
            m_actions.ActiveSubject = [this]() { return m_activePage; };
        }

        [[nodiscard]] IAllocator& Allocator() const noexcept { return *m_allocator; }
        EditorContext(const EditorContext&) = delete;
        EditorContext& operator=(const EditorContext&) = delete;

        // === Events ===

        /// Open-pages list or active page changed.
        Function<void()> OnPagesChanged;
        /// Fired after project settings are saved (the settings dialog invokes it through
        /// NotifyProjectSettingsChanged), so session state derived from the settings, such as the
        /// default UI font and theme, applies again without reopening the project.
        Function<void()> OnProjectSettingsChanged;
        void NotifyProjectSettingsChanged()
        {
            if (OnProjectSettingsChanged)
            {
                OnProjectSettingsChanged();
            }
        }

        /// Request an incremental cook (wired by the application to its cook service). Pages
        /// call this after saving a builder-backed asset, so the cooked product (and every live
        /// proxy bound to it) refreshes without a manual Build > Cook All.
        Function<void(bool /*rebuild*/)> OnCookRequested;
        void RequestCook(bool rebuild = false);
        /// Whether a cook is running (wired by the application to the negation of
        /// EditorCookService::IsIdle). Pages that must not start against a half-written cooked
        /// database (play in editor) poll this and defer. Unwired (tests, no project) means never
        /// busy.
        Function<bool()> CookBusy;
        [[nodiscard]] bool IsCookBusy() const { return CookBusy ? CookBusy() : false; }

        // === Pending asset edits (from viewport tools that edit an asset live, like sculpting) ===
        // A viewport tool edits a cooked product live, for immediate feedback, and registers a
        // closure that persists the edit back to its source asset. The editor's save flow drains
        // these, passing the source ContentDatabase it owns, and requests a recook on success. The
        // last edit per asset guid wins. This records what is dirty and how to persist it without
        // the framework knowing the domain: the closure lives in the domain's editor library
        // (Editor.Terrain), and it receives the database at drain time so it holds no handle to it.
        void RegisterAssetEdit(const Guid& assetId,
                               Function<Status(foundation::content::ContentDatabase&)> persist) override;
        [[nodiscard]] bool HasPendingAssetEdits() const noexcept
        {
            return !m_pendingAssetEdits.IsEmpty();
        }
        /// Runs and clears every pending edit, persisting through `db`; requests a recook if any
        /// succeeded.
        Status DrainAssetEdits(foundation::content::ContentDatabase& db);

        /// Transient status-bar text.
        Function<void(StringView)> OnStatus;

        /// Transient user-facing notification (a toast). Unwired, it falls back to the status bar,
        /// so pages can Notify unconditionally.
        Function<void(NoticeKind, StringView)> OnNotice;

        /// Open the asset with this Guid for editing. The application maps the Guid to its content
        /// Instance and takes the same page path a double-click in the browser takes. May be unset
        /// (callers check).
        Function<void(const Guid&)> OpenAsset;

        /// Asset-open interception: a claimant (such as the scene page, which takes animation clips
        /// into its animation bar) sees an OpenAsset before it routes to a page, and returning true
        /// claims it. Claimants are consulted newest first, so the most recently opened one wins;
        /// each checks its own visibility. Remove one with the returned id when the claimant is
        /// destroyed (pages unregister on close).
        using OpenAssetInterceptor = Function<bool(foundation::content::Instance&)>;
        u64 AddOpenAssetInterceptor(OpenAssetInterceptor interceptor)
        {
            const u64 id = ++m_nextInterceptorId;
            m_openInterceptors.PushBack(InterceptorEntry{id, Move(interceptor)});
            return id;
        }
        void RemoveOpenAssetInterceptor(u64 id)
        {
            for (usize i = 0; i < m_openInterceptors.Size(); ++i)
            {
                if (m_openInterceptors[i].id == id)
                {
                    m_openInterceptors.RemoveAt(i);
                    return;
                }
            }
        }
        [[nodiscard]] bool TryInterceptOpenAsset(foundation::content::Instance& instance)
        {
            for (usize i = m_openInterceptors.Size(); i > 0; --i) // newest-first
            {
                if (m_openInterceptors[i - 1].fn && m_openInterceptors[i - 1].fn(instance))
                {
                    return true;
                }
            }
            return false;
        }
        /// Thumbnail ready: a view other than the browser that shows asset thumbnails (the asset
        /// picker) hears when one finishes, so its row swaps the type icon for it at once rather
        /// than on its next rebind. The application forwards the thumbnail service's ready signal
        /// here. Remove the listener with the returned id when the view is destroyed.
        using ThumbnailListener = Function<void(const Guid&)>;
        u64 AddThumbnailListener(ThumbnailListener listener)
        {
            const u64 id = ++m_nextInterceptorId;
            m_thumbnailListeners.PushBack(ThumbnailEntry{id, Move(listener)});
            return id;
        }
        void RemoveThumbnailListener(u64 id)
        {
            for (usize i = 0; i < m_thumbnailListeners.Size(); ++i)
            {
                if (m_thumbnailListeners[i].id == id)
                {
                    m_thumbnailListeners.RemoveAt(i);
                    return;
                }
            }
        }
        [[nodiscard]] usize ThumbnailListenerCount() const noexcept
        {
            return m_thumbnailListeners.Size();
        }
        void NotifyThumbnailReady(const Guid& id)
        {
            for (const ThumbnailEntry& entry : m_thumbnailListeners)
            {
                if (entry.fn)
                {
                    entry.fn(id);
                }
            }
        }
        /// Reveal the asset with this Guid in the asset browser (select it and scroll it into view).
        Function<void(const Guid&)> RevealAsset;
        void Notify(NoticeKind kind, StringView message);
        /// An asset's display name for a reference row: its name, "(none)" for the nil id, and
        /// "(missing)" for an id the open project does not have (a dangling reference). Every row
        /// uses this spelling. The returned view is the instance's own name, valid while the
        /// instance lives.
        [[nodiscard]] StringView AssetNameFor(const Guid& id) const;

    private:
        IAllocator* m_allocator;
        struct InterceptorEntry
        {
            u64 id = 0;
            OpenAssetInterceptor fn;
        };
        Array<InterceptorEntry> m_openInterceptors;
        u64 m_nextInterceptorId = 0; // also numbers the thumbnail listeners
        struct ThumbnailEntry
        {
            u64 id = 0;
            ThumbnailListener fn;
        };
        Array<ThumbnailEntry> m_thumbnailListeners;

        struct PendingAssetEdit
        {
            Guid id;
            Function<Status(foundation::content::ContentDatabase&)> persist;
        };
        Array<PendingAssetEdit> m_pendingAssetEdits;

    public:

        // === Project ===

        /// The application owns the project; the context borrows it (null means no project open).
        void SetProject(EditorProject* project);

        // The application's background job runner (build lane and light lane). May be null in
        // headless and test contexts, where callers must fall back to synchronous work.
        [[nodiscard]] EditorJobService* Jobs() const noexcept { return m_jobs; }
        void SetJobs(EditorJobService* jobs) noexcept { m_jobs = jobs; }
        [[nodiscard]] ThumbnailService* Thumbnails() const noexcept { return m_thumbnails; }
        void SetThumbnails(ThumbnailService* thumbnails) noexcept { m_thumbnails = thumbnails; }
        [[nodiscard]] EditorProject* Project() const noexcept { return m_project; }

        // === Importers (an OS file into Sources/ and a typed Asset instance; registered by the exe) ===
        [[nodiscard]] pipeline::ImporterRegistry& Importers() noexcept { return m_importers; }

        // === Resources (runtime products over the project's cooked DB) ===
        // Owned by the application (created at project open). Pages resolve scene references and
        // the inspector's pickers bind through it. Null until a project is open.
        void SetResources(foundation::resource::ResourceManager* resources) noexcept;

        /// The per-project editor state store (<project>/Editor/: dock layout, favorites, open
        /// pages, per-page preferences). Borrowed; the application owns it and sets it for the
        /// lifetime of the open project (null between projects). Pages change their section and
        /// call MarkChanged, then RequestProjectEditorSettingsSave() to persist.
        void SetProjectEditorSettings(foundation::settings::Settings* store) noexcept
        {
            m_projectEditorSettings = store;
        }
        [[nodiscard]] foundation::settings::Settings* ProjectEditorSettings() const noexcept
        {
            return m_projectEditorSettings;
        }
        Function<void()> OnProjectEditorSettingsSaveRequested; // bound by the application to persist the store
        void RequestProjectEditorSettingsSave()
        {
            if (OnProjectEditorSettingsSaveRequested)
            {
                OnProjectEditorSettingsSaveRequested();
            }
        }

        /// The per-user editor settings store (<user-data>/editor.settings.xml). Borrowed; the
        /// application owns it and sets it at boot. Domains keep their own typed sections in it
        /// (Section<T>() and MarkChanged), so the application never needs to know their shapes.
        void SetUserEditorSettings(foundation::settings::Settings* store) noexcept
        {
            m_userEditorSettings = store;
        }
        [[nodiscard]] foundation::settings::Settings* UserEditorSettings() const noexcept
        {
            return m_userEditorSettings;
        }

        // --- domain-contributed editor settings -------------------------------------------
        // A domain (navigation, terrain and so on) registers a category of preference fields,
        // and the Preferences dialog renders every contribution generically, with nothing
        // hard-coded in the application. Fields carry get and set closures, so where the value
        // lives (usually the domain's own section in UserEditorSettings) stays the domain's
        // business. Only bool fields exist so far; add kinds as domains need them.
        struct EditorSettingsBoolField
        {
            String label;
            String description; // tooltip (empty for none)
            Function<bool()> get;
            Function<void(bool)> set;
        };
        struct EditorSettingsContribution
        {
            String category; // its Preferences tab ("Navigation"); one tab per category
            Array<EditorSettingsBoolField> bools;
        };
        void RegisterEditorSettingsContribution(EditorSettingsContribution contribution)
        {
            m_settingsContributions.PushBack(
                static_cast<EditorSettingsContribution&&>(contribution));
        }
        [[nodiscard]] const Array<EditorSettingsContribution>&
        EditorSettingsContributions() const noexcept
        {
            return m_settingsContributions;
        }

        /// Domain-contributed MCP tools: a domain's RegisterEditor (the scene editor, for one)
        /// registers what only it can serve over the live editor (the selection, simulate);
        /// the MCP host applies every contribution to its server when it starts. Registered at
        /// boot, before any host exists, like the settings contributions.
        using McpToolContribution = Function<void(foundation::mcp::McpServer&)>;
        void RegisterMcpToolContribution(McpToolContribution contribution)
        {
            m_mcpToolContributions.PushBack(static_cast<McpToolContribution&&>(contribution));
        }
        void ApplyMcpToolContributions(foundation::mcp::McpServer& server) const
        {
            for (const McpToolContribution& contribution : m_mcpToolContributions)
            {
                contribution(server);
            }
        }
        [[nodiscard]] usize McpToolContributionCount() const noexcept
        {
            return m_mcpToolContributions.Size();
        }
        [[nodiscard]] foundation::resource::ResourceManager* Resources() const noexcept;

        // === Registries ===

        [[nodiscard]] EditorPageRegistry& Pages() noexcept { return m_pageRegistry; }

        /// The editor's actions (see :actions): registered in the composition roots, every
        /// surface built from them, executed through them.
        [[nodiscard]] EditorActionRegistry& Actions() noexcept { return m_actions; }
        [[nodiscard]] const EditorActionRegistry& Actions() const noexcept { return m_actions; }


        /// The source asset types a resource reference to `product` takes (a picker's filter),
        /// wired by the application from its builders and the engine composition; empty when
        /// unwired (headless hosts, tests).
        Function<Array<const TypeInfo*>(const TypeInfo& product)> SourceAssetTypesOf;

        /// File > New's creators from every pipeline domain (pipeline::RegisterAllCreators fills
        /// the registry); the host runs one and does what follows a creation.
        [[nodiscard]] pipeline::AssetCreatorRegistry& Creators() noexcept { return m_creators; }
        [[nodiscard]] const pipeline::AssetCreatorRegistry& Creators() const noexcept
        {
            return m_creators;
        }

        // === Favorites (pinned asset instances, which the browser and picker show first) ===

        [[nodiscard]] bool IsFavorite(const Guid& id) const;
        void ToggleFavorite(const Guid& id);
        [[nodiscard]] Span<const Guid> Favorites() const noexcept;
        void SetFavorites(Array<Guid> favorites) { m_favorites = Move(favorites); }
        /// Fired on every toggle (the application persists them to the project's Editor/ state).
        Function<void()> OnFavoritesChanged;

        // === Editor clipboard (shared across pages: entity subtrees, components) ===
        // One typed slot: `kind` says what the blob is ("entities", "component"); consumers
        // check the kind before parsing. Only an overwrite clears it.

        void SetClipboard(StringView kind, Array<byte> data);
        /// SetClipboard for a copy the user asked for: a toast says what was copied.
        void CopyToEditorClipboard(StringView kind, Array<byte> data, StringView what);
        /// Puts text on the OS clipboard and says so: every copy the user asks for announces
        /// itself ("Copied <what>"), so they know it worked. With no clipboard (a headless view) it
        /// warns instead. `clipboard` is borrowed and may be null.
        bool CopyText(foundation::gui::IClipboard* clipboard, StringView text, StringView what);
        [[nodiscard]] StringView ClipboardKind() const noexcept { return m_clipboardKind.AsView(); }
        [[nodiscard]] Span<const byte> ClipboardData(StringView kind) const noexcept;

        // === Open pages ===

        /// Open (or focus) a page editing `instance`: an existing page for the same instance is
        /// activated; otherwise the registry's nearest-type factory creates one. Null if no
        /// factory matches or the instance's type is not registered.
        EditorPage* OpenPage(foundation::content::Instance& instance);

        /// Adopt a page with no instance (the Game tab): the same ownership and active-page flow
        /// as OpenPage, but the caller constructs it (no instance, no factory dispatch).
        EditorPage* AdoptPage(UniquePtr<EditorPage> page);

        /// Close a page (the caller is responsible for save-prompting dirty pages first).
        void ClosePage(EditorPage* page);

        [[nodiscard]] Span<const UniquePtr<EditorPage>> OpenPages() const noexcept;

        /// The source asset `assetId` changed outside its page (apply to prefab, a regenerated
        /// model prefab or scene, an agent's write over MCP). Every open page editing it is told
        /// (EditorPage::OnAssetExternallyModified) and refreshes by its own rule. Returns how many
        /// pages were told.
        usize NotifyAssetExternallyModified(const Guid& assetId);

        [[nodiscard]] EditorPage* ActivePage() const noexcept { return m_activePage; }
        void SetActivePage(EditorPage* page);
        /// Makes `page` the active page and brings its panel to the front (OnRevealPage, which the
        /// application wires to its dock). SetActivePage alone never raises a background tab, and a
        /// hidden page's viewport never renders, so a tool that must show a page calls this.
        void RevealPage(EditorPage* page);
        /// Raises a page's panel so the user (and its viewport) can see it; the application
        /// wires this to its dock, since the context owns no panels. Unset in a headless context.
        Function<void(EditorPage*)> OnRevealPage;

        // === Selection ===

        /// Global asset selection (the asset browser and instance pickers). Entity selection
        /// belongs to each scene page.
        [[nodiscard]] Selection<const foundation::content::Instance*>& AssetSelection() noexcept;

        // === Import notifications ===

        /// Subscribe to successful file imports (fired by the import flow after the importer
        /// returns; `options` is the options object the dialog edited, null when none). Used for
        /// post-import steps that live above the importer's layer, such as generating a prefab
        /// from a model, which needs scene machinery the importer library never links.
        void AddImportListener(
            Function<void(foundation::content::Instance&, const pipeline::ImportOptions*)> listener);

        /// Play in editor: creates a Game tab, which runs the player's behaviour in a tab.
        /// Registered by the scene editor plugin; when unset, the Game menu item shows a notice.
        /// `newInstance` false reuses the application's primary GameInstance (the normal Play);
        /// true starts an additional instance (multi-instance play in editor).
        /// `pieId` names the tab: the id its dock panel persists under and the PIE tools address
        /// it by (`game-page`, then `game-page-1`, ...).
        Function<UniquePtr<EditorPage>(bool newInstance, StringView pieId)> GamePageFactory;

        /// Export hook: transcodes a scene or prefab instance's text source stream to the binary
        /// format for staging. Registered by the scene editor plugin (it needs scene machinery
        /// editor.core never links). Returns false for non-scene instances or on failure; the
        /// exporter then stages the source as it is, and the runtime detects the format. Main
        /// thread only (it creates a scratch scene through the SceneSubsystem).
        Function<bool(foundation::content::Instance&, Array<byte>&)> SceneStreamStager;

        /// Export reachability hook: collects the assets a scene or prefab instance references, for
        /// the export closure. It loads the instance over the application's full manager set,
        /// appending its component resource Ref ids to `outResources` and its nested prefab
        /// instance ids to `outPrefabs`; returns false for non-scene instances or on failure.
        /// Registered by the scene editor plugin (it needs scene machinery). Main thread only (it
        /// loads a scene through the SceneSubsystem), which is why the editor scans with it on the
        /// main thread first and hands the resulting guid set to the background export job.
        Function<bool(foundation::content::Instance&, foundation::content::ContentDatabase&,
                      Array<Guid>& /*outResources*/, Array<Guid>& /*outPrefabs*/)>
            SceneRefScanner;

        void NotifyImported(foundation::content::Instance& instance, const pipeline::ImportOptions* options);

        // === Script breakpoints (the debugger) ===
        // Shared editor state: the ScriptPage gutter toggles them per source file and line, and a
        // Game run applies them to its script debugger. Plain data with no debugger contract, so
        // a remote debugger could consume the same set.

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
        // Set by a Game run's debugger listener on Breakpoint or Stepped (the innermost stack
        // frame), and cleared on resume or stop. The ScriptPage editing that file shows it as the
        // ExecutionLine marker. Version-stamped so consumers can poll it cheaply each frame.

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
        /// unknown name). Installed by the Game page for the run's lifetime; the probe itself
        /// checks the pause state. Like the breakpoint store it carries no debugger contract, so
        /// a remote debugger could install the same shape.
        Function<String(StringView)> ScriptValueProbe;

        // === Status ===

        void SetStatus(StringView text);

    private:
        void NotifyPagesChanged();

        EditorProject* m_project = nullptr;
        EditorJobService* m_jobs = nullptr; // borrowed (owned by the application)
        ThumbnailService* m_thumbnails = nullptr; // borrowed (owned by the application)
        foundation::resource::ResourceManager* m_resources = nullptr; // borrowed (owned by the application)
        foundation::settings::Settings* m_projectEditorSettings = nullptr; // borrowed (owned by the application)
        foundation::settings::Settings* m_userEditorSettings = nullptr;    // borrowed (owned by the application)
        Array<EditorSettingsContribution> m_settingsContributions;
        Array<McpToolContribution> m_mcpToolContributions;
        pipeline::ImporterRegistry m_importers;                               // borrowed
        EditorPageRegistry m_pageRegistry;
        EditorActionRegistry m_actions;
        pipeline::AssetCreatorRegistry m_creators;
        String m_clipboardKind;
        Array<byte> m_clipboard;
        Array<Guid> m_favorites;
        Array<Function<void(foundation::content::Instance&, const pipeline::ImportOptions*)>> m_importListeners;
        Array<UniquePtr<EditorPage>> m_pages;
        EditorPage* m_activePage = nullptr;
        Selection<const foundation::content::Instance*> m_assetSelection;
        Array<ScriptBreakpoint> m_breakpoints; // shared script debugger breakpoints
        ScriptExecutionPoint m_executionPoint; // paused debugger location (versioned)
        u64 m_executionPointVersion = 0;
    };
}
