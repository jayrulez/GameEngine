// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::App - :application partition.
//
// EditorApplication: the editor as a runtime IApplication - the
// UISandbox wiring, assembled for real: TrueType font service + UIHost + RuntimeDockableWindowHost
// (floating panels = borderless OS windows, drag-follow Tick) + the EditorShell chrome on the main
// window, with the EditorContext + EditorProject from editor.core underneath. Opens (or
// scaffolds) the project directory on startup, restores the per-user dock layout, saves it on
// shutdown.

module;
#define _CRT_SECURE_NO_WARNINGS
#include "Core/Prelude.h"
#include "Core/Log/Log.h"
#include <cstdlib>

export module editor.app:application;

import foundation.core;
import foundation.shell;
import foundation.graphics;
import foundation.fonts;
import foundation.fonts.truetype;
import foundation.runtime;
import foundation.runtime.client;
import foundation.vfs;    // NativeFileSystem: the editor's mount over the data root
import engine.defaultapp; // the embedded game application (v3)
import editor.mcp;        // ProjectSession + the operations the MCP host serves through
import :mcp_host;         // EditorMcpHost (per project, pumped per frame)
import :mcp_operations;   // EditorProjectOperations (the host's cook / import / export)
import :mcp_page_tools;   // the page tools the host adds over this application's pages
import :mcp_window_tools; // editor_screenshot, over this application's windows
import :window_capture;   // EditorWindowCapture (the tool, View > Screenshot, --screenshot)
import :mcp_action_tools; // the action bridge, unattended
import foundation.ui.resource;        // UITheme (the manifest's default game-UI theme)
import engine.ui;       // UISubsystem (SetDefaultTheme)
import engine.input;    // InputSubsystem (the embedded runtime's scene-input policy)
import foundation.render.api;
import foundation.ui;
import foundation.ui.toolkit;
import foundation.ui.runtime;
import foundation.ui.application;
import foundation.content;
import foundation.vfs;
import foundation.resource;
import pipeline.core;
import editor.core;
import editor.preview; // ThumbnailStage (the GPU thumbnail renderer, app-owned)
import foundation.settings;
import :assets_view;
import :editor_icons;
import :settings_dialog;
import :preferences_dialog;
import :command_palette;
import :project_manager_view;
import :shell;
import :font_atlas_cache;
import :ui_page;
import :action_menus;     // ActionMenuBar (the bar generated from the action registry)
import :action_shortcuts; // ActionShortcuts (the globals generated from it)

using namespace foundation::core;
using namespace pipeline;

export namespace editor::app
{
    namespace runtime = foundation::runtime;
    namespace graphics = foundation::graphics;
    namespace fonts = foundation::fonts;
    namespace ui = foundation::ui;

    class EditorApplication;

    struct EditorAppConfig
    {
        // THE data root (resolved by main - --data-root or the Data/.dataroot walk). The editor
        // mounts it once: the UI host's shaders, the embedded runtime, and the export's shader
        // cook all read from it. Required.
        String dataRoot;
        String projectDirectory; // opened on startup; scaffolded if no manifest yet
        String projectName = String(u8"Untitled"); // name used when scaffolding
        String fontPath;     // UI font (.ttf); empty = no text (debug only)
        String monoFontPath; // fixed-pitch font (.ttf) for code editors; empty = no Mono family
                             // (CodeEditView then falls back to the default family)
        // The LAST-RESORT face, baked into the exe at build time: loads when both the
        // user's preference path and the dev-tree path fail (a relocated editor). Null =
        // no embedded fallback (the editor may come up textless, loudly).
        const u8* embeddedFont = nullptr;
        usize embeddedFontSize = 0;

        // Log capture registered on GlobalLogger by main() BEFORE anything else runs, so early
        // startup logs reach the console panel. Borrowed; main owns it (outlives the app).
        editor::EditorLogBuffer* logBuffer = nullptr;

        // Start on the PROJECT MANAGER screen instead of opening projectDirectory (set by
        // main() when no project was given). File > Close Project returns to the manager only
        // in this mode; a CLI-opened editor keeps its single-project lifecycle.
        bool startInProjectManager = false;
        // CLI: when the project directory has no manifest and gets scaffolded, also seed the
        // starter content the manager's New Project flow seeds (baseline font/sky/primitive
        // meshes). Headless regeneration of that content - `Tools.Editor <dir> --seed`.
        bool seedOnScaffold = false;

        // Smoke-test aid: request a CLEAN shutdown after this many seconds (0 = never).
        // Exercises the real teardown path, unlike killing the process.
        f32 autoExitSeconds = 0.0f;
        // Smoke-test aid: trigger Build > Rebuild All after this many seconds (0 = never).
        // Exercises the hot-reload cascade exactly like the menu click.
        f32 autoRebuildSeconds = 0.0f;
        // Smoke-test aid: the MAIN window's backbuffer written as a PNG to this path (empty =
        // never), once screenshotAfterSeconds have run - so a run proves what it drew (the UI
        // included, since the capture records after the UI host's draw). `Tools.Editor
        // --screenshot <png> --screenshot-after <s>`; the same ScreenshotCapture as the runtime's.
        String screenshotPath;
        f32 screenshotAfterSeconds = 0.0f;
        // The MCP host for THIS run: `--mcp` enables it regardless of the preference and
        // `--mcp-port <n>` picks its port (0 = the preference's), so an agent that launches
        // the editor itself needs no UI. EditorMcpSettings is the persisted preference.
        bool mcpEnabled = false;
        u32 mcpPort = 0;

        // The assembly seams - editor.app never links engine modules or the
        // editor plugin modules; the EXECUTABLE composes them here:
        /// Called from IApplication::Configure - register engine subsystems (scene/render/...).
        Function<void(runtime::IApplicationHost&)> configureEngine;
        /// Called at the end of OnStartup - per-subsystem RegisterEditor entry points, plus
        /// wiring the app to engine INTERFACES it drives (app.SetSceneRenderer(...)).
        Function<void(EditorApplication&, runtime::IApplicationHost&, ui::runtime::UIHost&)>
            registerEditors;
        /// Seeds STARTER CONTENT into a project the manager just created (baseline font +
        /// sky + primitive meshes, and the manifest defaults that reference them). Runs
        /// once, right after the fresh project opens; the exe composes it because only the
        /// exe links every asset type. Null = new projects start empty.
        Function<void(editor::EditorContext&, editor::EditorProject&)>
            seedNewProject;

        /// Startup's progress, for the splash main() shows until the editor is ready: what it is
        /// doing now and how far along (0..1). Null = no splash.
        Function<void(StringView, f32)> startupProgress;
        /// Called once at the end of OnStartup, the editor ready: main() closes the splash and
        /// shows the main window (created hidden while the splash is up). Null = nothing to do.
        Function<void()> startupFinished;
    };

    class EditorApplication : public runtime::IApplication
    {
    public:
        explicit EditorApplication(EditorAppConfig config) : m_config(Move(config))
        {
            // Install the tagged Editor root for the free-helper seam (providers,
            // gizmos, icon caches - code with no owner parameter to thread).
            editor::SetEditorRootAllocator(m_editorAllocator);
            // Thumbnails must be reachable from the CONSTRUCTOR on: Tools.Editor's
            // registration block (where each domain's Register<X>Editor folds its
            // thumbnail generator in) runs before the UI-boot phase.
            m_context.SetThumbnails(&m_thumbnailService);
        }

        [[nodiscard]] editor::EditorContext& Context() noexcept { return m_context; }
        [[nodiscard]] editor::EditorProject* Project() const noexcept;
        [[nodiscard]] EditorShell& Shell() noexcept { return m_shell; }
        /// The exe registers every engine builder here (from registerEditors), mirroring the
        /// Tools.Cook CLI's set - the cook service routes through it.
        [[nodiscard]] pipeline::BuilderRegistry& Builders() noexcept { return m_builders; }
        [[nodiscard]] editor::EditorCookService& CookService() noexcept;

        /// The ResourceManager over the project's cooked DB; its factories are the embedded
        /// runtime's (the engine composition's set, registered on attach).
        [[nodiscard]] foundation::resource::ResourceManager* Resources() const noexcept;
        /// The embedded game application (valid after OnStartup; the Game page drives its
        /// play bracket through it).
        [[nodiscard]] engine::runtime::DefaultApplication* EmbeddedApplication() const noexcept;

        void Configure(runtime::IApplicationHost& host) override;

        /// The renderer interface the app drives its per-frame scene bracket through (injected
        /// by the exe from registerEditors; null = no scene rendering). Borrowed.
        void SetSceneRenderer(foundation::render::ISceneRenderer* renderer) noexcept;

        void OnStartup(runtime::IApplicationHost& host) override;
        /// Tell the splash what startup is doing (EditorAppConfig::startupProgress), while it is
        /// starting; a project opened later reports nothing.
        void ReportStartup(StringView status, f32 progress);

        /// Opening a page over uncooked content queues a scoped cook: every resource id
        /// that was requested during the page's resolve but has no product (the
        /// ResourceManager caches a null-product handle for each miss, and product guid ==
        /// source guid, so those ids are the exact missing-dependency roots), plus the
        /// page's own asset when its cook is missing/failed. Fully cooked pages request
        /// nothing - no worker spin-up on every open (six restored pages = six no-op cooks
        /// otherwise).
        void CookMissingForPage(foundation::content::Instance& instance);

        /// Open (or focus) the singleton Game tab (play-in-editor: the player behavior
        /// in-process; the page's own toolbar runs Play/Stop). Created through the scene
        /// editor plugin's factory seam - editor.app never links scene modules.
        // `newInstance` = false: the normal Play (focus the existing tab or open one on the primary
        // instance). true: "Play New Instance" - an ADDITIONAL Game tab driving its own GameInstance
        // (multi-instance PIE).
        void OpenGamePage(bool newInstance = false);

        /// Open (or focus) a page for `instance` and dock its content as a center tab.
        UIEditorPage* OpenInstancePage(foundation::content::Instance& instance);

        // Tear down a page whose panel is closing/closed (the DockManager owns panel
        // destruction; this handles only the page side).
        /// Maps a context notice to a toast (errors stick until closed; the rest self-expire).
        void ShowToast(editor::NoticeKind kind, StringView message);
        // Bind the project's default UI theme + font onto the embedded app's game UI.
        // Idempotent; called at project open, after every finished cook (a fresh
        // checkout's first cook creates the products the open-time bind missed), and on
        // settings save (a changed default takes effect without a reopen).
        void ApplyProjectUiDefaults();

        /// Save `subject` (the action file.save over its subject page) and flush the pending
        /// asset edits; the outcome as a notice.
        void SavePage(editor::EditorPage& subject);
        void FlushPendingAssetEdits(); // drain tool-registered live asset edits to source + recook

        void ClosePage(UIEditorPage* page);

        void OnUpdate(runtime::IApplicationHost& host, f32 dt) override;

        void OnRenderWindow(runtime::IApplicationHost& host, graphics::FrameContext& frame) override;

        void OnShutdown(runtime::IApplicationHost&) override;

    private:
        // File > New <creator>: create the source instance, remember it as the project's default
        // document if none is set yet (so a fresh project reopens where you left off), open it.
        void CreateAndOpen(const pipeline::AssetCreator& creator, foundation::content::Group* group = nullptr);
        /// What follows every creation, File > New's and asset_create's alike: a first scene
        /// becomes the project's default, the browser shows the new row, and a type a builder
        /// cooks is cooked so it is pickable at once.
        void AfterCreate(const pipeline::AssetCreator& creator,
                         foundation::content::Instance& instance);

        // True = nothing dirty, exit may proceed. Otherwise shows the exit prompt and returns
        // false; its buttons finish the job (save-all -> exit / discard -> exit / cancel).
        [[nodiscard]] bool ConfirmExitAllowed();

        static void AppendCountTo(String& out, usize value);

        // === Export ===

        // Build a fresh export template registry + presets and run one preset (or all) via the shared
        // driver - the SAME ExportOne/ExportAll the Tools.Export CLI calls. The host template comes
        // from this editor's own Bin dir (where Engine.Player + its .runtime-libs live). (Imported
        // cross-platform templates land with the templates-manager UI; host-platform export works now.)
        // Export runs in TWO safe phases so the UI never freezes: (1) cook through the CookService
        // (background + DB-safe - the cook mutates the DB the UI reads), then (2) once the cook finishes
        // (polled in OnUpdate), a background JobService job packs/stages/player (reader-only, cook=false).
        void RunExport(StringView presetName, bool all);

        // Phase 2: pack/stage/player as a background job (the cook already ran). File I/O only - no
        // source/cooked DB MUTATION - so it is safe alongside the main thread's DB reads.
        // Load per-user editor settings from <userdata>/editor.settings.xml (registering the section
        // types first). Absent on first run - the store stays empty and sections read as defaults.
        // Recursive walk feeding the export pre-transcode (main thread; scene/prefab typed
        // instances only - the stager filters).

        void LoadEditorSettings();

        // The editor's export templates root: the EditorExportSettings override when set, else
        // $ENV_TEMPLATES_DIR, else the <user-data>/templates default (same order as the CLI).
        [[nodiscard]] String TemplatesRoot() const;

        // Absolute form of `path`, resolved against the CWD. The project directory can be relative,
        // but the folder-reveal and clean logs need an absolute path (a relative one would reveal a
        // nonexistent path). Already-absolute paths pass through unchanged; if the CWD can't be read,
        // PathJoin degrades to the original relative path.
        [[nodiscard]] static String Absolutize(StringView path);

        // Does this export run include a preset that prunes to reachable? (m_exportPresets must be
        // loaded first.) Decides whether the main-thread reachability pre-scan is worth running.
        [[nodiscard]] bool AnyPresetPrunes(bool all, StringView presetName) const;

        void SubmitExportJob(String presetName, bool all);

        // === Templates + presets UI (Project > Export... / Edit > Export Templates...) ===
        //
        // Two management surfaces over the shared export driver: a templates manager (list + import /
        // create / remove installed bundles, with an engine-version match note) and an export-presets
        // panel (list + add / edit / duplicate / delete + Export / Export All). The presets panel drives
        // a preset-editor form; both defer every view-destroying action (dialog swap, list refresh)
        // through the UI mutation queue, and copy any async native-dialog paths before touching the UI.
        // The non-UI logic lives in ExportPresetsController + the template registry/helpers (tested).

        // Build a template registry on the MAIN thread: the host template (this editor's own Bin dir)
        // plus imported/created bundles under the configured templates root. Self-contained after
        // Refresh (copies manifests + dir paths), so it outlives the temporary filesystems here.
        void BuildTemplateRegistryMainThread(editor::TemplateRegistry& out);

        // Persist the in-memory preset set to the project's export_presets.xml.
        void SavePresetsController();

        // The export templates dialog (TemplatesDialog), over whatever is open: its seams are the
        // registry this host sees, the templates root, the OS folder picker and file manager.
        // Returns it, so a caller (Export) can follow its closing; null without a UI.
        ui::Dialog* OpenTemplatesManager();

        // The export dialog (ExportDialog) over the project's presets, reloaded from its
        // export_presets.xml: its seams save them, run an export, open the templates dialog over it
        // and pick extra files.
        void OpenExportPresetsPanel();

        // Save As: write the page's CURRENT content to a NEW asset beside the original and
        // rebind the page to it. The original keeps its on-disk state - the escape hatch when
        // an asset changed under a dirty page (apply-to-prefab) and both versions matter.
        void SavePageAs(editor::EditorPage& subject);

        void ShowDirtyCloseDialog(UIEditorPage* page, ui::toolkit::DockablePanel* panel);

        // Tab titles mirror dirty state (" *" suffix) - polled per frame; SetTitle no-ops
        // visually unless the string actually changed. The name comes from the LIVE instance
        // (the page captured it at open - a browser rename would leave the tab stale).
        void SyncPageTitles();

        // Buffered engine logs -> the Console panel, once per frame on the main thread.
        void DrainLog();

        // === Project lifecycle (the built-in project manager opens/closes at runtime) ===

        // Open + wire a project end-to-end: manifest (with the CLI scaffold fallback),
        // favorites, resources (late-attached to the embedded runtime), cook service +
        // assets view, saved pages + dock layout, and the recent-projects registry touch.
        // Swaps the window to the editor shell when the manager was showing.
        void OpenProjectAt(StringView directory);

        // The inverse of OpenProjectAt: saves layout/pages, closes every page, shuts the
        // cook service down, detaches resources from the embedded runtime, releases the
        // project, and returns to the manager screen. Pages must already be clean/confirmed
        // (ConfirmCloseProjectThen is the UI entry).
        void CloseProject();

        // The MCP host rides the project: started (when the preference or --mcp enables it)
        // once the project's services are up, stopped before they go.
        void StartMcpHost();
        void StopMcpHost();

        // Show the manager screen (building it on first use); swaps the window root.
        void EnterManagerMode();

        // Manager entries: version-relation prompt (backup-then-upgrade / newer-engine
        // warning) then OpenProjectAt; scaffold a new project then open it.
        void OpenFromManager(StringView directory);
        void CreateFromManager(StringView directory, StringView name);

        // File > Close Project: dirty-pages prompt, then CloseProject.
        void ConfirmCloseProjectThen();


        void SaveLayout();

        void RegisterActions();
        void BuildMenus();
        void RefreshActivePageMark(); // the active page's panel carries the dock's ActiveMark

        // I4 instrumentation: log the ResourceManager's live-product report (counts by type;
        // unreferenced = cache-only purge candidates). Project > Report Resource Memory.
        void ReportResourceMemory();

        // The editor's root: everything the editor allocates (UI tree, pages, panels,
        // services) rolls up under the Editor memory tag. Declared FIRST in this block
        // so every member below may thread it.
        TaggedAllocator m_editorAllocator{editor::EditorRootAllocator(),
                                          RegisterMemoryTag("Editor")};

        // Disk cache for the startup MSDF font bake (installed into
        // FontAtlasBakerFactory in OnStartup, cleared in OnShutdown).
        UniquePtr<editor::FontAtlasDiskCache> m_fontAtlasCache;

        EditorAppConfig m_config;
        runtime::IApplicationHost* m_host = nullptr; // borrowed
        foundation::render::ISceneRenderer* m_sceneRenderer = nullptr;
        // The embedded runtime (v3): gameplay subsystems + ALL scene hosting live here.
        runtime::Context m_runtimeContext{editor::EditorRootAllocator()}; // editor process root
        UniquePtr<runtime::EmbeddedApplicationHost> m_embeddedHost;
        // (the transport pumps on NetworkSubsystem::PostUpdate)
        UniquePtr<engine::runtime::DefaultApplication> m_embeddedApp;

        // Log drain state (see DrainLog).
        Array<editor::EditorLogEntry> m_pendingLog;
        u64 m_logSequence = 0;

        // Open pages and their center-tab panels (panels owned by the DockManager).
        struct PagePanel
        {
            UIEditorPage* page = nullptr;                // borrowed (context owns the page)
            ui::toolkit::DockablePanel* panel = nullptr; // borrowed (dock manager owns the panel)
        };
        /// Closes a page and its panel together, synchronously: the code paths (Close Project, an
        /// open asset deleted, page_close), where the tab close runs the same pair deferred. The
        /// page's views borrow what the page owns (its scene, its edit context, its images), so
        /// they must die while the page lives; the panel's deletion is deferred, so the panel
        /// lets go of its content FIRST, and the page then frees its views in its own teardown.
        void ClosePanelAndPage(PagePanel entry);
        editor::EditorContext m_context{m_editorAllocator};
        UniquePtr<editor::EditorProject> m_project;
        pipeline::BuilderRegistry m_builders{m_editorAllocator}; // exe-assembled (registerEditors)
        editor::EditorCookService m_cookService;
        // The MCP host and what it serves through, per project: the session points at the open
        // project, the operations run cook / import / export on this application's services
        // (the cook service, the job service - the paths the menus take), the host last.
        editor::mcp::ProjectSession m_mcpSession;
        UniquePtr<EditorProjectOperations> m_mcpOperations;
        UniquePtr<EditorMcpHost> m_mcpHost; // after the services are up, gone before they go
        editor::ThumbnailService m_thumbnailService; // per-project state
        UniquePtr<editor::ThumbnailStage> m_thumbnailStage; // GPU half (per project, app-driven)
        editor::EditorJobService m_jobService{m_editorAllocator}; // background jobs (export, ...)
        foundation::settings::Settings m_editorSettings{
            editor::EditorRootAllocator()}; // per-user editor prefs (<userdata>/editor.settings.xml)
        editor::ProjectManagerController m_projectManager{
            m_editorSettings}; // headless manager decisions (open gate, registry, create)
        UniquePtr<foundation::settings::Settings>
            m_projectEditorSettings; // per-project editor state (<project>/Editor/); one store,
                                     // typed sections (dock layout, favorites, open pages, ...)
        struct PendingExport
        {
            String presetName;
            bool all = false;
            bool waitingCook = false;
            bool active = false;
        };
        PendingExport m_pendingExport;
        editor::ExportPresetsController
            m_presetsController; // backs the Export presets panel + editor form
        HashMap<Guid, Array<byte>> m_exportSceneStreams; // export pre-transcoded scene wires
        editor::ExportPresetSet m_exportPresets; // main-thread-loaded presets for the running job
        Array<Guid> m_exportReachableRoots; // main-thread pre-scan result (reachable closure roots)
        bool m_exportReachableValid =
            false;                          // true when the pre-scan ran (else no pruning this run)
        UIEditorPage* m_gamePage = nullptr; // the PRIMARY game tab (focus target); extras untracked
        u32 m_gamePageCounter = 0;          // unique persistence id for "Play New Instance" tabs
        f32 m_elapsed = 0.0f;               // autoExit/autoRebuild/screenshot accumulator
        // Screenshots of the editor's windows: --screenshot, editor_screenshot, View > Screenshot.
        EditorWindowCapture m_windowCapture{m_editorAllocator};
        bool m_screenshotFired = false;    // the --screenshot one shot is armed
        bool m_starting = false; // OnStartup is running: its steps go to the splash
        ui::DrawablePtr m_logo;  // the AssiduousEngine logo (LoadEditorLogo), for the Welcome page and the manager
        bool m_announceScreenshot = false; // View > Screenshot: tell the user when it is written

        // The ids of the windows that can draw a frame now, the main window first; empty while
        // the main window is minimised (nothing to capture).
        [[nodiscard]] Array<u32> CapturableWindows();
        // View > Screenshot (F12): the main window to a new PNG under <user-data>/screenshots.
        void TakeEditorScreenshot();
        f32 m_testOpenElapsed = 0.0f;       // ENV_TEST_OPEN hook
        u32 m_testOpenStage = 0;
        bool m_autoRebuilt = false;
        Array<foundation::shell::DroppedFile> m_droppedFiles; // per-frame drain buffer
        UniquePtr<foundation::resource::ResourceManager> m_resources;
        // The project's native game module, loaded against the EMBEDDED runtime context
        // for the project's lifetime (game-native-code.md N2; null = none/failed/static).
        UniquePtr<foundation::runtime::PluginHost> m_gamePlugins;
        u32 m_nativeReloadCount = 0; // versions the hot-reload copies (N6)

        // Hot reload (game-native-code.md N6): stop the run, scope-reversed unload
        // (leaking the old mapping on purpose), load a FRESH VERSIONED COPY of the
        // module (dlopen refcounts by path - reloading the same file returns the old
        // mapping), OnLoad re-registers.
        void ReloadNativeModule();

        // Copies the project's declared native module to .cache/native-hot/reload-N-<base>
        // and returns that path (empty on failure - the caller reports). EVERY load goes
        // through this, including the first at project open, for two reasons: dlopen
        // refcounts by path (so a reload of the same path returns the stale mapping), and
        // on Windows LoadLibraryW holds the image file open against writes - mapping the
        // declared path directly would make "Build Native Module" fail to relink it for as
        // long as the project stays open (game-native-code.md N5/N6, shared-libraries.md W3).
        [[nodiscard]] foundation::core::String StageNativeModuleCopy();

        UniquePtr<fonts::TrueTypeFontService> m_fontService;
        ui::toolkit::ToolkitThemeExtension m_toolkitTheme;
        RefPtr<foundation::ui::StyleSheet> m_styleSheet;

        // TEARDOWN ORDER RULE: the UIHost (owns the UIContext + InputManager) is declared
        // BEFORE every view-holding member below, so it destructs AFTER them - view teardown
        // calls DetachView/Unregister on its context (LogView's ListView does, via
        // SetAdapter(nullptr) in its dtor), which is a use-after-free once the host is gone.
        // ASAN caught exactly that with the previous declared-last ordering.
        UniquePtr<foundation::vfs::NativeFileSystem> m_dataFileSystem; // the data mount (owned)
        UniquePtr<ui::runtime::UIHost> m_uiHost;
        // Icon bake state: the content scale the icon set was last baked for (OnUpdate
        // re-bakes when the main window's scale drifts - monitor moves, OS scale change).
        void BakeEditorIcons(f32 contentScale);
        f32 m_iconBakeScale = 0.0f;
        UniquePtr<ProjectManagerView> m_managerView; // built on first EnterManagerMode
        bool m_seedAfterOpen = false; // starter-content request from CreateFromManager
        bool m_inManagerMode = false;
        UniquePtr<ui::application::RuntimeDockableWindowHost>
            m_dockHost; // references m_uiHost: dies first
        EditorShell m_shell;
        UniquePtr<ActionMenuBar> m_actionMenus;       // the menu bar, from the registry
        UniquePtr<ActionShortcuts> m_actionShortcuts; // the global shortcuts, from it
        RefPtr<AssetsView> m_assetsView;
        RefPtr<ui::toolkit::ToastHost> m_toastHost;
        Array<PagePanel> m_pagePanels;
    };
}
