// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::App - :application partition.
//
// EditorApplication: the editor as a runtime IApplication. It keeps the lifecycle alone
// (Documentation/Specs/editor-gui.md, 5.1, layer 3): it configures the engine, starts its parts
// in order, hands each frame to them and shuts them down. The parts are its own files: the window
// and its GUI (EditorWindow), the open project (EditorSession), the native module, export, asset
// creation, the MCP host, and the editor-wide actions. Underneath are the EditorContext and the
// services from editor.core, and the EMBEDDED runtime every scene, preview and Game tab is hosted
// in: the same DefaultApplication the player runs.
module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"

export module editor.app:application;

import foundation.core;
import foundation.shell;
import foundation.graphics;
import foundation.runtime;
import foundation.runtime.client;
import foundation.render.api;
import foundation.content;
import foundation.resource;
import foundation.settings;
import pipeline.core;
import engine.defaultapp; // the embedded game application
import editor.core;
import :window_capture;   // EditorWindowCapture (View > Screenshot, --screenshot, the MCP tool)
import :editor_window;
import :editor_dock;
import :pages;
import :action_menus;
import :action_shortcuts;
import :command_palette;
import :preferences_dialog;
import :picker;
import :group_picker_dialog;
import :resource_ref_editor;
import :asset_page;
import :code_languages;
import :settings_dialog;
import :templates_dialog;
import :export_dialog;
import :project_home;
import :project_manager_view;
import :log_view;
import :profiler_panel;
import :assets_view;
import :asset_operations;
import :asset_import;
import :import_dialog;
import :native_module;
import :export_service;
import :asset_creation;
import :session;
import :mcp_service;

using namespace foundation::core;

export namespace editor::app
{
    namespace runtime = foundation::runtime;
    namespace graphics = foundation::graphics;

    class EditorApplication;

    struct EditorAppConfig
    {
        // THE data root (resolved by main - --data-root or the Data/.dataroot walk). The editor
        // mounts it once: the GUI's shaders and the editor's files, the embedded runtime, and the
        // export's shader cook all read from it. Required.
        String dataRoot;
        String projectDirectory; // opened on startup; scaffolded if no manifest yet
        String projectName = String(u8"Untitled"); // name used when scaffolding

        // Log capture registered on GlobalLogger by main() BEFORE anything else runs, so early
        // startup logs reach the console. Borrowed; main owns it (outlives the app).
        editor::EditorLogBuffer* logBuffer = nullptr;

        // Start on the PROJECT MANAGER screen instead of opening projectDirectory (set by
        // main() when no project was given).
        bool startInProjectManager = false;
        // CLI: when the project directory has no manifest and gets scaffolded, also seed the
        // starter content the manager's New Project flow seeds - `Tools.Editor <dir> --seed`.
        bool seedOnScaffold = false;

        // Smoke-test aid: request a CLEAN shutdown after this many seconds (0 = never).
        f32 autoExitSeconds = 0.0f;
        // Smoke-test aid: trigger Build > Rebuild All after this many seconds (0 = never).
        f32 autoRebuildSeconds = 0.0f;
        // Smoke-test aid: the MAIN window's backbuffer written as a PNG to this path (empty =
        // never), once screenshotAfterSeconds have run, the GUI included (the capture records
        // after the GUI draws).
        String screenshotPath;
        f32 screenshotAfterSeconds = 0.0f;
        // `--screenshot-exit`: the clean shutdown once that capture is written (or has failed).
        bool screenshotExit = false;
        // The MCP host for THIS run: `--mcp` enables it whatever the preference says and
        // `--mcp-port <n>` picks its port (0 = the preference's).
        bool mcpEnabled = false;
        u32 mcpPort = 0;

        // The assembly seams - editor.app never links engine modules or the editor plugin
        // modules; the EXECUTABLE composes them here:
        /// Called from IApplication::Configure - register engine subsystems (scene/render/...).
        Function<void(runtime::IApplicationHost&)> configureEngine;
        /// Called during OnStartup - per-subsystem RegisterEditor entry points, plus wiring the
        /// app to engine INTERFACES it drives (app.SetSceneRenderer(...)). The host is the
        /// EMBEDDED one: every page's scenes live there.
        Function<void(EditorApplication&, runtime::IApplicationHost&)> registerEditors;
        /// Seeds STARTER CONTENT into a project the manager just created; null = new projects start empty.
        Function<void(editor::EditorContext&, editor::EditorProject&)> seedNewProject;

        Function<void(StringView, f32)> startupProgress;
        Function<void()> startupFinished;
    };

    class EditorApplication : public runtime::IApplication
    {
    public:
        explicit EditorApplication(EditorAppConfig config);
        ~EditorApplication() override;

        [[nodiscard]] editor::EditorContext& Context() noexcept { return m_context; }
        [[nodiscard]] editor::EditorProject* Project() const noexcept { return m_session.Project(); }
        [[nodiscard]] pipeline::BuilderRegistry& Builders() noexcept { return m_builders; }
        [[nodiscard]] editor::EditorCookService& CookService() noexcept { return m_cookService; }
        [[nodiscard]] foundation::resource::ResourceManager* Resources() const noexcept { return m_session.Resources(); }
        [[nodiscard]] engine::runtime::DefaultApplication* EmbeddedApplication() const noexcept { return m_embeddedApp.Get(); }
        [[nodiscard]] EditorWindow& Window() noexcept { return m_window; }
        /// What an asset slot or a page's picker asks of the editor: the accepted types, the
        /// project's names, the picker (made the first time).
        [[nodiscard]] const ResourceRefServices& RefServices();
        /// Opens the picker over the window for `request`, in its middle.
        void ShowPicker(PickerRequest request);
        /// Opens the asset picker (its groups beside what each holds) over the window for `request`.
        void ShowAssetPicker(AssetPickRequest request);
        /// Opens Create Asset over the window, titled `title` ("New Clip"), the group `group` (null:
        /// the project's top) and the name `suggested` to start; `create` is told the group and the
        /// name chosen (the caller makes the asset).
        void ShowCreateAsset(StringView title, StringView suggested, foundation::content::Group* group,
                             Function<void(foundation::content::Group&, StringView)> create);
        /// A type's icon, asked of the type (made the first time, with the slots' table).
        [[nodiscard]] editor::EditorIcons& TypeIcons()
        {
            (void)RefServices();
            return *m_typeIcons;
        }
        /// What an asset page asks of the editor when it is made: its markup, the asset slots'
        /// table, the type icons, the host and the window's GUI a preview renders in.
        [[nodiscard]] AssetPageServices PageServices()
        {
            AssetPageServices services;
            EditorWindow* window = &m_window;
            services.loadMarkup = [window](StringView path) { return window->LoadMarkup(path); };
            services.refs = &RefServices();
            services.icons = &TypeIcons();
            services.host = m_embeddedHost.Get(); // the runtime's, with its scenes and renderer (the scene page's too)
            services.gui = m_window.Host();
            services.languages = &m_codeLanguages;
            EditorApplication* self = this;
            services.pick = [self](PickerRequest request) { self->ShowPicker(Move(request)); };
            return services;
        }
        /// The one source every page factory asks for PageServices (the registrations take it by
        /// reference; it lives as long as the editor).
        [[nodiscard]] const AssetPageServicesSource& PageServicesSource() noexcept { return m_pageServices; }
        [[nodiscard]] EditorDock& Dock() noexcept { return m_dock; }
        [[nodiscard]] EditorPages& Pages() noexcept { return *m_pages; }
        [[nodiscard]] EditorAssetCreation& AssetCreation() noexcept { return m_creation; }

        void SetSceneRenderer(foundation::render::ISceneRenderer* renderer) noexcept { m_sceneRenderer = renderer; }

        /// The main window presents as every editor window does (EditorRenderWindowDesc).
        [[nodiscard]] foundation::graphics::RenderWindowDesc MainRenderWindow() const override { return EditorRenderWindowDesc(); }
        void Configure(runtime::IApplicationHost& host) override;
        void OnStartup(runtime::IApplicationHost& host) override;
        void OnUpdate(runtime::IApplicationHost& host, f32 dt) override;
        void OnRenderWindow(runtime::IApplicationHost& host, graphics::FrameContext& frame) override;
        void OnShutdown(runtime::IApplicationHost& host) override;

        /// A toast, as every notice and job result is said.
        void ShowToast(editor::NoticeKind kind, StringView message, StringView actionText = {}, Function<void()> action = {})
        {
            m_window.ShowToast(kind, message, actionText, Move(action));
        }

    private:
        void ReportStartup(StringView status, f32 progress);
        void LoadEditorSettings();
        void StartEmbeddedRuntime(runtime::IApplicationHost& host);
        void RegisterActions();
        void AddNativeCode();
        void BuildNativeModule();
        void ShowAbout();
        void OpenDock();
        void OpenPages();
        void BuildMenus();
        void OpenCommandPalette();
        bool ConfirmExit();
        void OpenPreferences();
        void OpenProjectSettings();
        /// Imports files into a group: at once when nothing needs review, else through the window.
        void ImportFiles(Span<const String> paths, foundation::content::Group& group);
        /// Export Templates over the main window; `closed` hears when it closes.
        void OpenTemplates(Function<void()> closed = {});
        void OpenExport();
        [[nodiscard]] foundation::shell::IDialogService* ShellDialogs() const;
        /// Shows a picker's request in the middle of the main window.
        void EnterManagerMode();
        void OpenFromManager(StringView directory);
        void CreateFromManager(StringView directory, StringView name);
        bool OpenProject(StringView directory, bool seed);
        void CloseProjectToManager();
        void Later(Function<void()> work);
        void DrainLog();
        void OpenDefaultScene(editor::EditorProject& project);
        [[nodiscard]] EditorAssetCreation::Services CreationServices();
        [[nodiscard]] EditorSession::Services SessionServices();
        [[nodiscard]] Array<u32> CapturableWindows();
        void TakeEditorScreenshot();
        void UpdateCaptures(runtime::IApplicationHost& host, f32 dt);
        void UpdateStatusBar();

        TaggedAllocator m_editorAllocator{editor::EditorRootAllocator(), RegisterMemoryTag("Editor")};
        EditorAppConfig m_config;
        runtime::IApplicationHost* m_host = nullptr; // borrowed
        foundation::render::ISceneRenderer* m_sceneRenderer = nullptr;
        runtime::Context m_runtimeContext{editor::EditorRootAllocator()}; // the editor process's root
        UniquePtr<runtime::EmbeddedApplicationHost> m_embeddedHost;
        UniquePtr<engine::runtime::DefaultApplication> m_embeddedApp;

        editor::EditorContext m_context{m_editorAllocator};
        pipeline::BuilderRegistry m_builders{m_editorAllocator}; // exe-assembled (registerEditors)
        editor::EditorCookService m_cookService;
        editor::EditorJobService m_jobService{m_editorAllocator}; // background jobs (export, ...)
        editor::ThumbnailService m_thumbnailService;               // per-project state
        foundation::settings::Settings m_editorSettings{editor::EditorRootAllocator()}; // per-user (<userdata>/editor.settings.xml)
        editor::ProjectManagerController m_projectManager{m_editorSettings}; // the registry of projects

        // The parts, in the order they start; they stop in reverse.
        EditorWindow m_window{m_editorAllocator};
        AssetPageServicesSource m_pageServices; // the page factories', reading the window above (made in the constructor)
        CodeLanguages m_codeLanguages;          // read through the window's resources (made in the constructor)
        EditorDock m_dock{m_editorAllocator};
        UniquePtr<EditorPages> m_pages; // made with the dock, in OnStartup
        UniquePtr<ActionMenuBar> m_menus; // the frame's menu bar, from the actions
        UniquePtr<ActionShortcuts> m_shortcuts; // the actions' keys in the GUI's keymap
        UniquePtr<CommandPalette> m_palette;    // every action by name (View > Command Palette)
        UniquePtr<EditorPreferencesDialog> m_preferences; // Edit > Preferences
        UniquePtr<editor::EditorIcons> m_typeIcons; // a type's icon, asked of the type
        UniquePtr<Picker> m_picker;                 // the one picker (paths, groups, components), made the first time
        UniquePtr<AssetPickerWindow> m_assetPicker; // an asset slot's picker, made the first time
        RefPtr<foundation::gui::Window> m_createAsset; // Create Asset, while open
        ResourceRefServices m_refs;                 // what an asset slot asks of the editor
        UniquePtr<ProjectSettingsDialog> m_projectSettings; // Project > Project Settings
        UniquePtr<TemplatesDialog> m_templates;             // Edit > Export Templates
        UniquePtr<ExportDialog> m_exportDialog;             // Project > Export (its project's presets)
        editor::EditorProject* m_exportProject = nullptr;   // the project m_exportDialog edits
        UniquePtr<ProjectHomeView> m_home; // the Project panel while a project is open
        UniquePtr<ProjectManagerView> m_manager; // the window's screen while no project is open
        UniquePtr<LogView> m_console;            // the Console panel's content
        UniquePtr<ProfilerPanel> m_profiler;     // the Profiler page's content
        UniquePtr<AssetOperations> m_assetOps;   // rename, duplicate, delete, cook: the browser's and an agent's
        UniquePtr<AssetsView> m_assets;          // the Assets panel's content
        UniquePtr<AssetImport> m_assetImport;    // files into the project (a drop, Import...)
        UniquePtr<ImportWindow> m_importWindow;  // the import's review, made the first time
        u64 m_logSequence = 0;                   // the last log entry the Console took
        EditorNativeModule m_native{m_editorAllocator};
        UniquePtr<EditorExport> m_export;
        EditorAssetCreation m_creation;
        UniquePtr<EditorMcpService> m_mcp;
        EditorSession m_session; // the open project (after the services it uses, before nothing it does)

        EditorWindowCapture m_windowCapture{m_editorAllocator};
        f32 m_elapsed = 0.0f;            // autoExit, autoRebuild and screenshot accumulator
        bool m_screenshotFired = false;  // the --screenshot one shot is armed
        bool m_announceScreenshot = false; // View > Screenshot: tell the user when it is written
        bool m_autoRebuilt = false;
        bool m_starting = false; // OnStartup is running: its steps go to the splash
    };
}
