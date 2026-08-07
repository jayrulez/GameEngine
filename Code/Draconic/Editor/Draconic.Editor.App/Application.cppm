// Draconic::EditorApp - :application partition.
//
// EditorApplication: the editor as a runtime IApplication (docs/design/editor.md §3.2) - the
// UISandbox wiring, assembled for real: TrueType font service + UIHost + RuntimeDockableWindowHost
// (floating panels = borderless OS windows, drag-follow Tick) + the EditorShell chrome on the main
// window, with the EditorContext + EditorProject from draconic.editor.core underneath. Opens (or
// scaffolds) the project directory on startup, restores the per-user dock layout, saves it on
// shutdown. Phase 1: chrome + project only; pages/panels grow in later phases.

module;
#define _CRT_SECURE_NO_WARNINGS
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Log/Log.h"
#include <cstdlib>

export module draconic.editor.app:application;

import draconic.foundation;
import draconic.shell;
import draconic.graphics;
import draconic.fonts;
import draconic.fonts.ttf;
import draconic.runtime;
import draconic.runtime.client;
import draconic.engine.defaultapp; // the embedded game application (v3)
import draconic.ui.resource;        // UITheme (the manifest's default game-UI theme)
import draconic.engine.ui;       // UISubsystem (SetDefaultTheme)
import draconic.engine.input;    // InputSubsystem (the embedded runtime's scene-input policy)
import draconic.render.api;
import draconic.ui;
import draconic.ui.toolkit;
import draconic.ui.runtime;
import draconic.ui.application;
import draconic.content;
import draconic.vfs;
import draconic.resource;
import draconic.editor;
import draconic.editor.core;
import draconic.settings;
import :assets_view;
import :editor_icons;
import :settings_dialog;
import :preferences_dialog;
import :project_manager_view;
import :shell;
import :ui_page;

using namespace draconic::foundation;

export namespace draconic::editor::app
{
    namespace runtime = draconic::runtime;
    namespace graphics = draconic::graphics;
    namespace fonts = draconic::fonts;
    namespace ui = draconic::ui;
    namespace editor = draconic::editor;

    class EditorApplication;

    struct EditorAppConfig
    {
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
        draconic::editor::EditorLogBuffer* logBuffer = nullptr;

        // Start on the PROJECT MANAGER screen instead of opening projectDirectory (set by
        // main() when no project was given). File > Close Project returns to the manager only
        // in this mode; a CLI-opened editor keeps its single-project lifecycle.
        bool startInProjectManager = false;

        // Smoke-test aid: request a CLEAN shutdown after this many seconds (0 = never).
        // Exercises the real teardown path, unlike killing the process.
        f32 autoExitSeconds = 0.0f;
        // Smoke-test aid: trigger Build > Rebuild All after this many seconds (0 = never).
        // Exercises the hot-reload cascade exactly like the menu click.
        f32 autoRebuildSeconds = 0.0f;

        // The assembly seams (design doc §3.1) - editor.app never links engine modules or the
        // draconic.<sys>.editor plugin modules; the EXECUTABLE composes them here:
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
        Function<void(draconic::editor::EditorContext&, draconic::editor::EditorProject&)>
            seedNewProject;
    };

    class EditorApplication : public runtime::IApplication
    {
    public:
        explicit EditorApplication(EditorAppConfig config) : m_config(Move(config)) {}

        [[nodiscard]] draconic::editor::EditorContext& Context() noexcept { return m_context; }
        [[nodiscard]] draconic::editor::EditorProject* Project() const noexcept;
        [[nodiscard]] EditorShell& Shell() noexcept { return m_shell; }
        /// The exe registers every engine builder here (from registerEditors), mirroring the
        /// Draconic.Tools.Cook CLI's set - the cook service routes through it.
        [[nodiscard]] draconic::editor::BuilderRegistry& Builders() noexcept { return m_builders; }
        [[nodiscard]] draconic::editor::EditorCookService& CookService() noexcept;

        /// The exe registers runtime resource factories here (from registerEditors); the app
        /// owns them + the ResourceManager over the project's cooked DB.
        void AddResourceFactory(UniquePtr<draconic::resource::IResourceFactory> factory);
        [[nodiscard]] draconic::resource::ResourceManager* Resources() const noexcept;
        /// The embedded game application (valid after OnStartup; the Game page drives its
        /// play bracket through it).
        [[nodiscard]] runtime::DefaultApplication* EmbeddedApplication() const noexcept;

        void Configure(runtime::IApplicationHost& host) override;

        /// The renderer interface the app drives its per-frame scene bracket through (injected
        /// by the exe from registerEditors; null = no scene rendering). Borrowed.
        void SetSceneRenderer(draconic::render::ISceneRenderer* renderer) noexcept;

        void OnStartup(runtime::IApplicationHost& host) override;

        /// Opening a page over uncooked content queues a scoped cook: every resource id
        /// that was requested during the page's resolve but has no product (the
        /// ResourceManager caches a null-product handle for each miss, and product guid ==
        /// source guid, so those ids are the exact missing-dependency roots), plus the
        /// page's own asset when its cook is missing/failed. Fully cooked pages request
        /// nothing - no worker spin-up on every open (six restored pages = six no-op cooks
        /// otherwise).
        void CookMissingForPage(draconic::content::Instance& instance);

        /// Open (or focus) the singleton Game tab (play-in-editor: the player behavior
        /// in-process; the page's own toolbar runs Play/Stop). Created through the scene
        /// editor plugin's factory seam - editor.app never links scene modules.
        // `newInstance` = false: the normal Play (focus the existing tab or open one on the primary
        // instance). true: "Play New Instance" - an ADDITIONAL Game tab driving its own GameInstance
        // (multi-instance PIE, game-instance.md §11 step 5).
        void OpenGamePage(bool newInstance = false);

        /// Open (or focus) a page for `instance` and dock its content as a center tab.
        UIEditorPage* OpenInstancePage(draconic::content::Instance& instance);

        // Tear down a page whose panel is closing/closed (the DockManager owns panel
        // destruction; this handles only the page side).
        /// Maps a context notice to a toast (errors stick until closed; the rest self-expire).
        void ShowToast(editor::NoticeKind kind, StringView message);

        void SaveActivePage();

        void ClosePage(UIEditorPage* page);

        void OnUpdate(runtime::IApplicationHost& host, f32 dt) override;

        void OnRenderWindow(runtime::IApplicationHost& host, graphics::FrameContext& frame) override;

        void OnShutdown(runtime::IApplicationHost&) override;

    private:
        // File > New <creator>: create the source instance, remember it as the project's default
        // document if none is set yet (so a fresh project reopens where you left off), open it.
        void CreateAndOpen(const draconic::editor::EditorContext::AssetCreator& creator, draconic::content::Group* group = nullptr);

        // True = nothing dirty, exit may proceed. Otherwise shows the exit prompt and returns
        // false; its buttons finish the job (save-all -> exit / discard -> exit / cancel).
        [[nodiscard]] bool ConfirmExitAllowed();

        static void AppendCountTo(String& out, usize value);

        // === Export ===

        // Build a fresh export template registry + presets and run one preset (or all) via the shared
        // driver - the SAME ExportOne/ExportAll the Draconic.Tools.Export CLI calls. The host template comes
        // from this editor's own Bin dir (where Draconic.Engine.Player + its .runtime-libs live). (Imported
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
        void CollectSceneStreams(draconic::content::Group& group);

        void LoadEditorSettings();

        // The editor's export templates root: the EditorExportSettings override when set, else
        // $DRACONIC_TEMPLATES_DIR, else the <user-data>/templates default (same order as the CLI).
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

        // === Templates + presets UI (File > Export... / File > Manage Templates...) ===
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

        // A labeled form row: fixed-width label + `field` (grows to fill). Returns the row so a caller
        // can append trailing controls (e.g. a "Browse..." button beside a text field).
        ui::FlexLayout* AddFormRow(ui::FlexLayout& column, StringView label, ui::View* field);

        // Swap the currently-open management dialog for a freshly-built one: close `current` and run
        // `open`, all on the UI mutation queue (never destroy/rebuild views mid-event-dispatch).
        void QueueReplaceDialog(ui::Dialog* current, Function<void()> open);

        void ReopenExportPresetsPanel(ui::Dialog* current);
        void ReopenTemplatesManager(ui::Dialog* current);

        // Persist the in-memory preset set to the project's export_presets.xml.
        void SavePresetsController();

        // Join / split the additionalFiles list <-> the ";"-separated text of the editor's Extra Files
        // field (an EditText holds no array, so the form marshals through a single string).
        [[nodiscard]] static String JoinSemicolons(const Array<String>& items);
        static void SplitSemicolons(StringView text, Array<String>& out);

        // Native multi-select "open file" -> append the chosen absolute paths to the Extra Files field.
        // `target` is held by RefPtr so the field survives even if the form closes before the async
        // dialog resolves (SetText on a detached view is harmless); no view hierarchy is rebuilt.
        void PickAdditionalFiles(RefPtr<ui::EditText> target);

        // Import a template bundle (folder with a template.xml) into the templates root, then rebuild
        // the manager. Async: the picked path is copied into ImportTemplate before any UI mutation.
        void ImportTemplateThenRefresh(ui::Dialog* current);

        // Create a template bundle from a "Bin/<Config>/<Platform>-<Compiler>" build dir (packaging the
        // player + its runtime-libs), installing it into the templates root, then rebuild the manager.
        void CreateTemplateThenRefresh(ui::Dialog* current);

        // Templates manager: list every registry template (imported + the synthesized host), each with
        // its platform/config/engine-version and a soft "(!) engine mismatch" note; Import / Create /
        // Remove (non-host only) mutate the templates root and rebuild this dialog.
        void OpenTemplatesManager();

        // Export presets panel: (re)load the project's export_presets.xml into the controller, list each
        // preset with per-row Export / Edit / Duplicate / Delete, plus Add / Export All / Manage
        // Templates in the footer. Edit/Add open the preset-editor form (swapping this dialog).
        void OpenExportPresetsPanel();

        // Preset-editor form: name, a template dropdown (registry.All(): sets templateId + derives
        // platform/config) OR explicit platform/config when "(resolve by ...)" is chosen, player name,
        // output subdir, additionalFiles (native multi-select picker) and the stageSymbols /
        // pruneToReachable toggles. Save writes through the controller (Add when `editIndex` < 0, else
        // Update), persists, and returns to the presets panel; Cancel just returns.
        void OpenPresetEditor(editor::ExportPreset initial, isize editIndex);

        // Save As: write the page's CURRENT content to a NEW asset beside the original and
        // rebind the page to it. The original keeps its on-disk state - the escape hatch when
        // an asset changed under a dirty page (apply-to-prefab) and both versions matter.
        void SaveActivePageAs();

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

        // Show the manager screen (building it on first use); swaps the window root.
        void EnterManagerMode();

        // Manager entries: version-relation prompt (backup-then-upgrade / newer-engine
        // warning) then OpenProjectAt; scaffold a new project then open it.
        void OpenFromManager(StringView directory);
        void CreateFromManager(StringView directory, StringView name);

        // File > Close Project: dirty-pages prompt, then CloseProject.
        void ConfirmCloseProjectThen();


        void SaveLayout();

        void BuildMenus();

        EditorAppConfig m_config;
        runtime::IApplicationHost* m_host = nullptr; // borrowed
        draconic::render::ISceneRenderer* m_sceneRenderer = nullptr;
        // The embedded runtime (v3): gameplay subsystems + ALL scene hosting live here.
        runtime::Context m_runtimeContext;
        UniquePtr<runtime::EmbeddedApplicationHost> m_embeddedHost;
        runtime::FixedStepper
            m_embeddedFixedStepper; // drives the embedded app's OnFixedUpdate (net) in-editor
        UniquePtr<runtime::DefaultApplication> m_embeddedApp;
        bool m_stopGameRequested = false; // borrowed (exe injects)

        // Log drain state (see DrainLog).
        Array<draconic::editor::EditorLogEntry> m_pendingLog;
        u64 m_logSequence = 0;

        // Open pages and their center-tab panels (panels owned by the DockManager).
        struct PagePanel
        {
            UIEditorPage* page = nullptr;                // borrowed (context owns the page)
            ui::toolkit::DockablePanel* panel = nullptr; // borrowed (dock manager owns the panel)
        };
        draconic::editor::EditorContext m_context;
        UniquePtr<draconic::editor::EditorProject> m_project;
        draconic::editor::BuilderRegistry m_builders; // exe-assembled (registerEditors)
        draconic::editor::EditorCookService m_cookService;
        draconic::editor::EditorJobService m_jobService; // generic background jobs (export, ...)
        draconic::settings::Settings
            m_editorSettings; // per-user editor prefs (<userdata>/editor.settings.xml)
        draconic::editor::ProjectManagerController m_projectManager{
            m_editorSettings}; // headless manager decisions (open gate, registry, create)
        UniquePtr<draconic::settings::Settings>
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
        f32 m_elapsed = 0.0f;               // autoExit/autoRebuild accumulator
        f32 m_testOpenElapsed = 0.0f;       // DRACONIC_TEST_OPEN hook
        u32 m_testOpenStage = 0;
        bool m_autoRebuilt = false;
        Array<draconic::shell::DroppedFile> m_droppedFiles; // per-frame drain buffer
        Array<UniquePtr<draconic::resource::IResourceFactory>> m_resourceFactories; // exe-assembled
        UniquePtr<draconic::resource::ResourceManager> m_resources;

        UniquePtr<fonts::TrueTypeFontService> m_fontService;
        ui::toolkit::ToolkitThemeExtension m_toolkitTheme;
        RefPtr<draconic::ui::StyleSheet> m_styleSheet;

        // TEARDOWN ORDER RULE: the UIHost (owns the UIContext + InputManager) is declared
        // BEFORE every view-holding member below, so it destructs AFTER them - view teardown
        // calls DetachView/Unregister on its context (LogView's ListView does, via
        // SetAdapter(nullptr) in its dtor), which is a use-after-free once the host is gone.
        // ASAN caught exactly that with the previous declared-last ordering.
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
        RefPtr<AssetsView> m_assetsView;
        RefPtr<ui::toolkit::ToastHost> m_toastHost;
        Array<PagePanel> m_pagePanels;
    };
}
