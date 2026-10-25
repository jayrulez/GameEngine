// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::App - EditorApplication's lifecycle (Documentation/Specs/editor-gui.md, 5.1, layer 3):
// configure, start the parts in order, hand each frame to them, shut them down in reverse. What
// each part does is its own file; what is here is the order and the wiring between them.
module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"

module editor.app;

import foundation.core;
import foundation.shell;
import foundation.graphics;
import foundation.runtime;
import foundation.runtime.client;
import foundation.render.api;
import foundation.content;
import foundation.resource;
import foundation.settings;
import foundation.gui; // MessageBox (About)
import pipeline.core;
import engine.defaultapp;
import engine.input; // the embedded runtime's scene-input policy
import engine.ui;
import engine.gui;
import editor.core;

using namespace foundation::core;

// The build identity compiled into Runtime.Client: the version About shows. Resolved at the final link.
extern "C" const char* BuildStamp();

namespace editor::app
{
    EditorApplication::EditorApplication(EditorAppConfig config)
        : m_config(Move(config)), m_creation(CreationServices()), m_session(m_editorAllocator, SessionServices())
    {
        editor::SetEditorRootAllocator(m_editorAllocator);
        m_context.SetThumbnails(&m_thumbnailService);
    }

    EditorApplication::~EditorApplication() = default;

    EditorAssetCreation::Services EditorApplication::CreationServices()
    {
        EditorAssetCreation::Services services;
        services.context = &m_context;
        services.cook = &m_cookService;
        services.builders = &m_builders;
        EditorApplication* self = this;
        services.openPage = [self](foundation::content::Instance& instance) { (void)self->m_pages->Open(instance); };
        return services;
    }

    EditorSession::Services EditorApplication::SessionServices()
    {
        EditorSession::Services services;
        services.context = &m_context;
        services.cook = &m_cookService;
        services.jobs = &m_jobService;
        services.builders = &m_builders;
        services.thumbnails = &m_thumbnailService;
        services.userSettings = &m_editorSettings;
        services.projects = &m_projectManager;
        services.runtime = &m_runtimeContext;
        services.native = &m_native;
        EditorApplication* self = this;
        services.seedNewProject = [self](editor::EditorContext& context, editor::EditorProject& project)
        {
            if (self->m_config.seedNewProject)
                self->m_config.seedNewProject(context, project);
        };
        services.opened = [self](editor::EditorProject& project)
        {
            // The project's layout, else the default (the one before was another project's).
            foundation::settings::Settings* store = self->m_session.Store();
            if (store == nullptr || !ApplyDockLayout(self->m_dock.Controller(), *store).IsOk())
                self->m_dock.ResetLayout();
            // The pages from the last session (a restored layout kept their places), else the
            // project's default scene.
            const auto progress = [self](StringView name, f32 done)
            { self->ReportStartup(Format(u8"Opening {}", name).AsView(), 0.72f + 0.26f * done); };
            if (store == nullptr || !self->m_pages->Restore(*store, progress))
                self->OpenDefaultScene(project);
            if (self->m_export)
                self->m_export->SetProject(&project);
            self->m_creation.SetProject(&project);
            if (self->m_mcp)
                (void)self->m_mcp->Start(project); // the agent surface over this project, if enabled
        };
        services.closing = [self]()
        {
            self->m_pages->CloseAll(); // their resources while the project's services live
            if (self->m_mcp)
                self->m_mcp->Stop(); // no agent call may run against services that are going away
            if (self->m_export)
                self->m_export->SetProject(nullptr);
            self->m_creation.SetProject(nullptr);
        };
        services.captureState = [self](foundation::settings::Settings& store)
        {
            CaptureDockLayout(self->m_dock.Controller(), store);
            self->m_pages->Capture(store);
        };
        services.cookFinished = [self]()
        {
            // Failures stay until dismissed (the Console has the log); a cook of nothing says nothing.
            const usize failed = self->m_cookService.LastCookSummary().failed;
            const usize cooked = self->m_cookService.LastCookSummary().cooked;
            if (failed > 0)
                self->ShowToast(editor::NoticeKind::Error, Format(u8"Cook: {} failed, {} cooked (see Console).", failed, cooked).AsView());
            else if (cooked > 0)
                self->ShowToast(editor::NoticeKind::Success, Format(u8"Cook finished: {} asset(s).", cooked).AsView());
        };
        return services;
    }

    void EditorApplication::Configure(runtime::IApplicationHost& host)
    {
        if (m_config.configureEngine)
            m_config.configureEngine(host);
    }

    void EditorApplication::ReportStartup(StringView status, f32 progress)
    {
        if (m_starting && m_config.startupProgress)
            m_config.startupProgress(status, progress);
    }

    void EditorApplication::LoadEditorSettings()
    {
        editor::RegisterEditorSettingsTypes();
        editor::RegisterProjectRegistryTypes(); // the manager's recent projects
        RegisterEditorProjectSettingsTypes();   // the per-project store's sections
        const Status loaded = editor::LoadEditorSettingsFromUserData(m_editorSettings);
        if (!loaded.IsOk() && loaded.Code() != ErrorCode::NotFound)
            // NotFound (a first run) is fine; anything else is a partial load, which a later save
            // would write back without the sections it dropped (the project registry most painfully).
            LOG_ERROR(u8"Editor",
                      u8"editor.settings.xml load FAILED (partial store) - check for unregistered section types; later saves may "
                      u8"drop sections");
    }

    void EditorApplication::OnStartup(runtime::IApplicationHost& host)
    {
        m_host = &host;
        OpenPages(); // first: every frame hands them its hooks
        if (host.MainRenderWindow() == nullptr)
            return;
        m_starting = true; // the splash hears startup's steps until the editor is ready
        ReportStartup(u8"Loading settings", 0.12f);
        LoadEditorSettings();
        m_context.SetUserEditorSettings(&m_editorSettings);

        ReportStartup(u8"Preparing the interface", 0.30f);
        const EditorLook look = LookFrom(m_editorSettings.Find<editor::EditorUiSettings>());
        if (m_window.Open(host, m_config.dataRoot.AsView(), look))
            OpenDock();
        // The window's close button asks about unsaved pages first, as File > Exit does.
        if (host.Shell() != nullptr)
            host.Shell()->OnMainWindowCloseRequested = [self = this]() { return self->ConfirmExit(); };
        EditorApplication* self = this;
        m_context.OnNotice = [self](editor::NoticeKind kind, StringView message)
        {
            self->ShowToast(kind, message);
            self->m_context.SetStatus(message);
        };
        m_context.OnStatus = [self](StringView message) { self->m_window.SetStatus(message); };

        ReportStartup(u8"Starting the renderer", 0.45f);
        StartEmbeddedRuntime(host);
        m_session.SetEmbedded(m_embeddedHost.Get(), m_embeddedApp.Get());
        EditorExport::Services exportServices;
        exportServices.context = &m_context;
        exportServices.cook = &m_cookService;
        exportServices.jobs = &m_jobService;
        exportServices.builders = &m_builders;
        exportServices.userSettings = &m_editorSettings;
        exportServices.dataRoot = m_config.dataRoot;
        exportServices.onExported = [self](StringView folder)
        { self->ShowToast(editor::NoticeKind::Success, Format(u8"Exported to {}", folder).AsView()); };
        m_export = MakeUnique<EditorExport>(m_editorAllocator, m_editorAllocator, Move(exportServices));

        EditorMcpService::Services mcp;
        mcp.context = &m_context;
        mcp.cook = &m_cookService;
        mcp.jobs = &m_jobService;
        mcp.builders = &m_builders;
        mcp.userSettings = &m_editorSettings;
        mcp.log = m_config.logBuffer;
        mcp.dataRoot = m_config.dataRoot;
        mcp.forceEnabled = m_config.mcpEnabled;
        mcp.port = m_config.mcpPort;
        mcp.templatesRoot = [self]() { return self->m_export->TemplatesRoot(); };
        mcp.created = [self](const pipeline::AssetCreator& creator, foundation::content::Instance& instance)
        { self->m_creation.AfterCreate(creator, instance); };
        mcp.openPage = [self](const Guid& id) -> editor::EditorPage* { return self->m_pages->Open(id); };
        mcp.closePage = [self](editor::EditorPage* page) { self->m_pages->Close(page); };
        mcp.windows.windows = [self]() { return self->CapturableWindows(); };
        mcp.windows.request = [self](Span<const WindowShotRequest> requests) { self->m_windowCapture.Request(requests); };
        mcp.windows.shots = [self]() { return self->m_windowCapture.Shots(); };
        m_mcp = MakeUnique<EditorMcpService>(m_editorAllocator, m_editorAllocator, Move(mcp));

        // Per-subsystem editor plugins register here with the EMBEDDED host (every page's scenes
        // live there); once per run, since what they register outlives a project.
        ReportStartup(u8"Loading the editors", 0.62f);
        RegisterActions(); // the editor-wide set first: the menu bar follows registration order
        if (m_config.registerEditors)
            m_config.registerEditors(*this, *m_embeddedHost);
        // The user's shortcut overrides, once every domain has declared its actions.
        (void)editor::ApplyShortcutOverrides(m_editorSettings.Section<editor::EditorShortcutSettings>(), m_context.Actions());
        BuildMenus();

        if (m_config.startInProjectManager)
            // The project manager comes back with its port (X3, layer 13).
            m_context.SetStatus(u8"No project: pass one on the command line (the project manager is not on the GUI yet).");
        else
        {
            ReportStartup(Format(u8"Opening {}", PathFilename(m_config.projectDirectory.AsView())).AsView(), 0.70f);
            EditorSession::OpenOptions options;
            options.scaffold = true; // a command line's bare folder becomes a project
            options.scaffoldName = m_config.projectName;
            options.seed = m_config.seedOnScaffold;
            (void)m_session.Open(m_config.projectDirectory.AsView(), options);
        }

        ReportStartup(u8"Ready", 1.0f);
        m_starting = false;
        if (m_config.startupFinished)
            m_config.startupFinished();
    }

    // The editor owns a second, persistent runtime Context populated by the SAME DefaultApplication
    // the player runs: gameplay subsystems live there, and every scene (editing pages, Simulate,
    // previews, the Game tab) is hosted there. It starts with no resource manager: projects open
    // and close at runtime, so each project's manager attaches and detaches (EditorSession).
    void EditorApplication::StartEmbeddedRuntime(runtime::IApplicationHost& host)
    {
        m_embeddedHost = MakeUnique<runtime::EmbeddedApplicationHost>(m_editorAllocator, host, m_runtimeContext);
        m_embeddedHost->SetExitHandler(Function<void(int)>{[](int code)
                                                           {
                                                               // A Game tab routes its own instance's exit to its own run; what
                                                               // reaches here is an instance no tab is playing.
                                                               LOG_INFO(u8"Editor", u8"embedded app requested exit({}) with no Game tab playing it", code);
                                                           }});
        m_embeddedApp = MakeUnique<engine::runtime::DefaultApplication>(m_editorAllocator);
        m_embeddedApp->SetDataRoot(m_config.dataRoot.AsView()); // the editor's root, not a re-walk
        m_embeddedApp->Configure(*m_embeddedHost);
        // Un-bound input never reaches scene-tier game UI here: editing and Simulate HUDs render as
        // they will but do not take input; the Game tab is the interactive surface.
        if (m_embeddedApp->Input() != nullptr)
            m_embeddedApp->Input()->SetUnboundScenePolicy(engine::input::UnboundInputScenePolicy::ScreenTierOnly);
        // Each Game tab runs its own game in this one application, with screens of its own.
        if (m_embeddedApp->UI() != nullptr)
            m_embeddedApp->UI()->SetRunScreens(true);
        if (m_embeddedApp->GUI() != nullptr)
            m_embeddedApp->GUI()->SetRunScreens(true);
        m_runtimeContext.Startup();
        m_embeddedApp->OnStartup(*m_embeddedHost);
    }

    // The dock in the frame: the shared panels, their contents the window's markup (Assets and
    // Console a line each until their contents port, X4).
    void EditorApplication::OpenDock()
    {
        gui::Node* into = m_window.FindById(u8"frame-dock");
        if (into == nullptr)
        {
            LOG_ERROR(u8"Editor", u8"the frame has no frame-dock: the editor runs without its dock");
            return;
        }
        EditorApplication* self = this;
        const auto placeholder = [self](StringView text)
        {
            return [self, line = String(text)]()
            {
                RefPtr<gui::Label> label = MakeRef<gui::Label>(self->m_editorAllocator);
                label->AddClass(u8"panel-placeholder");
                label->SetText(line.AsView());
                return RefPtr<gui::Node>(label);
            };
        };
        EditorDock::Contents contents;
        contents.welcome = [self]() { return self->m_window.LoadMarkup(u8"Editor/Welcome.gml"); };
        contents.assets = placeholder(u8"The asset browser is not on the new interface yet.");
        contents.console = placeholder(u8"The console is not on the new interface yet.");
        m_dock.Open(*into, m_window.Host(), Move(contents));
    }

    // The pages in the dock; their dialogs over the main window.
    void EditorApplication::OpenPages()
    {
        EditorPages::Services services;
        services.context = &m_context;
        services.dock = &m_dock;
        services.cook = &m_cookService;
        EditorApplication* self = this;
        services.project = [self]() { return self->m_session.Project(); };
        services.resources = [self]() { return self->m_session.Resources(); };
        services.dialogRoot = [self]() { return self->m_window.Root(); };
        services.loadMarkup = [self](StringView path) { return self->m_window.LoadMarkup(path); };
        m_pages = MakeUnique<EditorPages>(m_editorAllocator, m_editorAllocator, Move(services));
    }

    // A project's first open: its default scene, by guid (else by path, for a manifest without one).
    void EditorApplication::OpenDefaultScene(editor::EditorProject& project)
    {
        foundation::content::Instance* instance = nullptr;
        if (!project.Settings().defaultSceneId.IsNil())
            instance = project.SourceDb().GetInstance(project.Settings().defaultSceneId);
        if (instance == nullptr && !project.Settings().defaultScene.IsEmpty())
            instance = project.SourceDb().GetInstance(project.Settings().defaultScene.AsView());
        if (instance != nullptr)
            (void)m_pages->Open(*instance);
    }

    // The frame's menu bar and keys from the action registry, File > New <creator> leading the
    // File menu (a creator with a category in a submenu of that name).
    void EditorApplication::BuildMenus()
    {
        auto* bar = Cast<gui::MenuBar>(m_window.FindById(u8"menubar"));
        if (bar == nullptr || m_window.Root() == nullptr)
            return; // no interface
        m_shortcuts = MakeUnique<ActionShortcuts>(m_editorAllocator, m_editorAllocator, m_window.Host()->Context().GetKeymap(), *m_window.Root(),
                                                  m_context.Actions());
        m_menus = MakeUnique<ActionMenuBar>(m_editorAllocator, m_editorAllocator, *bar, m_context.Actions());
        // The application's menus in their places; a domain's menu (Scene) or an extension's takes
        // the "*" slot, so Help stays last however late they register.
        const StringView order[] = {u8"File", u8"Edit", u8"Project", u8"Build", u8"Game", u8"View", u8"*", u8"Help"};
        m_menus->SetMenuOrder(Span<const StringView>(order, sizeof(order) / sizeof(order[0])));
        EditorApplication* self = this;
        m_menus->AddLeadingItems(u8"File", [self](gui::Menu& file)
        {
            Array<StringView> categories;
            for (const pipeline::AssetCreator& creator : self->m_context.Creators().All())
            {
                const pipeline::AssetCreator* entry = &creator;
                if (creator.category.IsEmpty())
                {
                    (void)file.AddItem(Format(u8"New {}", creator.label).AsView(), [self, entry]() { self->m_creation.CreateAndOpen(*entry); });
                    continue;
                }
                bool seen = false;
                for (StringView category : categories)
                    seen = seen || category == creator.category.AsView();
                if (!seen)
                    categories.PushBack(creator.category.AsView());
            }
            categories.Sort([](StringView a, StringView b) { return a.Compare(b) < 0; });
            for (StringView category : categories)
            {
                gui::Menu* submenu = file.AddSubMenu(category);
                for (const pipeline::AssetCreator& creator : self->m_context.Creators().All())
                    if (creator.category.AsView() == category)
                    {
                        const pipeline::AssetCreator* entry = &creator;
                        (void)submenu->AddItem(creator.label.AsView(), [self, entry]() { self->m_creation.CreateAndOpen(*entry); });
                    }
            }
        });
    }

    // The palette over the main window, its frame the editor's markup (made the first time).
    void EditorApplication::OpenCommandPalette()
    {
        gui::SceneRoot* root = m_window.Root();
        if (root == nullptr)
            return;
        if (!m_palette)
            m_palette = MakeUnique<CommandPalette>(m_editorAllocator, m_editorAllocator, m_context.Actions(), m_window.LoadMarkup(u8"Editor/CommandPalette.gml"));
        (void)m_palette->Open(*root);
    }

    // Preferences over the main window (made the first time), the look shown live in the window.
    void EditorApplication::OpenPreferences()
    {
        gui::SceneRoot* root = m_window.Root();
        if (root == nullptr)
            return;
        if (!m_preferences)
        {
            EditorPreferencesDialog::Services services;
            services.user = &m_editorSettings;
            services.actions = &m_context.Actions();
            EditorApplication* self = this;
            services.warn = [self](StringView text) { self->m_context.Notify(editor::NoticeKind::Warning, text); };
            services.contributions = &m_context.EditorSettingsContributions();
            services.mcpChanged = [self]()
            {
                // The host follows the saved section over the open project.
                if (!self->m_mcp || self->m_session.Project() == nullptr)
                    return;
                self->m_mcp->Stop();
                (void)self->m_mcp->Start(*self->m_session.Project());
            };
            services.showLook = [self](const EditorLook& look) { self->m_window.SetLook(look); };
            services.persist = [self]() { return editor::SaveEditorSettingsToUserData(self->m_editorSettings).IsOk(); };
            m_preferences = MakeUnique<EditorPreferencesDialog>(m_editorAllocator, m_editorAllocator, Move(services),
                                                                m_window.LoadMarkup(EditorPreferencesDialog::kPath));
        }
        (void)m_preferences->Open(*root);
    }

    // Exits once the unsaved pages are saved or let go; true when nothing needed asking.
    bool EditorApplication::ConfirmExit()
    {
        EditorApplication* self = this;
        return m_pages->ConfirmUnsavedThen(u8"Save All and Exit", u8"Exit Without Saving", [self]()
                                           {
                                               if (self->m_host != nullptr)
                                                   self->m_host->RequestExit();
                                           });
    }

    void EditorApplication::RegisterActions()
    {
        EditorActionRegistry& actions = m_context.Actions();
        EditorApplication* self = this;

        FileActions file;
        file.save = [self](editor::EditorPage& page) { self->m_pages->Save(page); };
        file.saveAs = [self](editor::EditorPage& page) { self->m_pages->SaveAs(page); };
        file.exit = [self]() { (void)self->ConfirmExit(); };
        RegisterFileActions(actions, Move(file));
        EditActions edit;
        edit.preferences = [self]() { self->OpenPreferences(); };
        RegisterEditActions(actions, Move(edit));

        ProjectActions project;
        project.context = &m_context;
        project.addNativeCode = [self]() { self->AddNativeCode(); };
        project.buildNativeModule = [self]() { self->BuildNativeModule(); };
        project.reloadNativeModule = [self]()
        { self->m_native.Reload(self->m_session.Project(), self->m_context, self->m_runtimeContext, self->m_session.Resources()); };
        project.reportResourceMemory = [self]() { EditorExport::ReportResourceMemory(self->m_session.Resources()); };
        RegisterProjectActions(actions, Move(project));

        BuildActions build;
        build.context = &m_context;
        build.cookAll = [self]() { self->m_cookService.RequestCook(false); };
        build.rebuildAll = [self]() { self->m_cookService.RequestCook(true); };
        RegisterBuildActions(actions, Move(build));

        GameActions game;
        game.context = &m_context;
        game.play = [self]() { self->m_pages->OpenGame(false); };
        game.playNewInstance = [self]() { self->m_pages->OpenGame(true); };
        RegisterGameActions(actions, Move(game));

        ViewActions view;
        view.resetLayout = [self]() { self->m_dock.ResetLayout(); };
        view.screenshot = [self]() { self->TakeEditorScreenshot(); };
        view.commandPalette = [self]() { self->OpenCommandPalette(); };
        RegisterViewActions(actions, Move(view));

        HelpActions help;
        help.about = [self]() { self->ShowAbout(); };
        RegisterHelpActions(actions, Move(help));
    }

    void EditorApplication::AddNativeCode()
    {
        editor::EditorProject* project = m_session.Project();
        if (project == nullptr)
            return;
        if (!project->Settings().nativeModule.IsEmpty())
        {
            m_context.Notify(editor::NoticeKind::Info, u8"This project already has native code (see Project Settings).");
            return;
        }
        const Status scaffolded = editor::ScaffoldNativeModule(*project);
        if (scaffolded.IsOk())
        {
            m_context.Notify(editor::NoticeKind::Success, u8"Native code scaffolded in Native/ - use Project > Build Native Module, then Reload.");
            LOG_INFO(u8"Editor", u8"native scaffold: Native/ created, manifest nativeModule = '{}'", project->Settings().nativeModule);
        }
        else if (scaffolded.Code() == ErrorCode::AlreadyExists)
            m_context.Notify(editor::NoticeKind::Warning, u8"Native/ already exists - not touching it (set Project Settings > Native module manually).");
        else
            m_context.Notify(editor::NoticeKind::Error, u8"Native scaffold failed (see Console).");
    }

    void EditorApplication::BuildNativeModule()
    {
        editor::EditorProject* project = m_session.Project();
        if (project == nullptr || project->Settings().nativeModule.IsEmpty())
        {
            m_context.Notify(editor::NoticeKind::Info, u8"No native module declared - Project > Add Native Code... first.");
            return;
        }
        editor::EditorContext* context = &m_context;
        m_jobService.Submit(u8"Native Build",
                            [context, project](editor::JobContext&) -> Status
                            {
                                const Status built = editor::BuildDevNativeModule(*project);
                                if (built.IsOk())
                                    context->Notify(editor::NoticeKind::Success, u8"Native module built - Project > Reload Native Module to pick it up.");
                                else
                                    context->Notify(editor::NoticeKind::Error, u8"Native build failed (see Console).");
                                return built;
                            });
    }

    // The version, in a message box over the editor.
    void EditorApplication::ShowAbout()
    {
        foundation::gui::SceneRoot* root = m_window.Root();
        if (root == nullptr)
            return;
        auto about = MakeRef<foundation::gui::MessageBox>(m_editorAllocator);
        about->Configure(u8"About AssiduousEngine", Format(u8"Version {}", StringView(reinterpret_cast<const char8_t*>(BuildStamp()))).AsView(),
                         foundation::gui::MessageBox::Buttons::Ok);
        about->OpenModal(*root);
    }

    void EditorApplication::OnUpdate(runtime::IApplicationHost& host, f32 dt)
    {
        if (m_mcp)
            m_mcp->Pump(); // a waiting agent call is answered here: tools touch main-thread state
        // The embedded runtime's frame first (fixed steps in BeginFrame, interpolation in Update):
        // pages then read fresh state. EndFrame closes after the scene bracket (OnRenderWindow).
        if (m_embeddedApp)
        {
            const f32 scaled = dt * m_runtimeContext.TimeScale();
            m_runtimeContext.BeginFrame(dt);
            m_runtimeContext.Update(scaled);
            m_runtimeContext.PostUpdate(scaled);
            // Every game instance's script ticks once a frame here, however many Game tabs.
            m_embeddedApp->OnUpdate(*m_embeddedHost, dt);
        }
        UpdateCaptures(host, dt);
        // The cook's and the jobs' progress to the status line; an export waiting on its cook starts.
        EditorApplication* self = this;
        m_cookService.Update(Function<void(StringView)>{[self](StringView line) { self->m_context.SetStatus(line); }});
        m_jobService.Update(Function<void(StringView)>{[self](StringView line) { self->m_context.SetStatus(line); }});
        if (m_export)
            m_export->Pump();
        UpdateStatusBar();
        if (foundation::resource::ResourceManager* resources = m_session.Resources())
        {
            resources->Pump();           // async loads finish
            resources->CollectGarbage(); // products a hot reload replaced go
        }
        m_window.Update(dt);
        m_pages->Update(host, dt); // after the GUI laid out: viewport rects are current
    }

    // The status bar's job (a job's title, how far, its step of how many; else a cook, of unknown
    // length) and agent (the MCP host while it listens).
    void EditorApplication::UpdateStatusBar()
    {
        EditorStatusBar* status = m_window.StatusBar();
        if (status == nullptr)
            return;
        const editor::EditorJobService::ProgressView progress = m_jobService.IsBusy() ? m_jobService.Progress() : editor::EditorJobService::ProgressView{};
        if (progress.active)
        {
            const String name = progress.step.IsEmpty() ? progress.title : Format(u8"{}: {}", progress.title, progress.step);
            const String count = progress.stepCount > 0 ? Format(u8"{} of {}", progress.stepIndex + 1, progress.stepCount) : String();
            status->SetJob(name.AsView(), progress.fraction, count.AsView());
        }
        else if (m_cookService.IsCooking())
            status->SetJob(u8"Cooking", -1.0f, {});
        else if (status->ShowsJob())
            status->ClearJob();
        const EditorMcpHost* host = m_mcp ? m_mcp->Host() : nullptr;
        status->SetAgent(host != nullptr && host->IsRunning() ? Format(u8"MCP on 127.0.0.1:{}", host->BoundPort()).AsView() : StringView{});
    }

    // Window screenshots: last frame's written, missing windows failed, the user told, and the
    // command line's timed shot, rebuild and exit.
    void EditorApplication::UpdateCaptures(runtime::IApplicationHost& host, f32 dt)
    {
        if (auto* gfx = host.Graphics(); gfx != nullptr && gfx->Raw() != nullptr)
            m_windowCapture.Complete(*gfx->Raw(), host.Ctx().Allocator());
        if (m_windowCapture.Busy())
        {
            Array<u32> open(m_editorAllocator);
            if (host.Shell() != nullptr && host.Shell()->WindowManager() != nullptr)
                for (foundation::shell::IWindow* window : host.Shell()->WindowManager()->Windows())
                    if (window != nullptr && window->IsOpen())
                        open.PushBack(window->Id());
            m_windowCapture.FailMissing(Span<const u32>(open.Data(), open.Size()));
        }
        if (m_announceScreenshot && !m_windowCapture.Busy())
        {
            m_announceScreenshot = false;
            const Array<WindowShot> shots = m_windowCapture.Shots();
            if (!shots.IsEmpty() && shots[0].state == WindowCaptureState::Written)
                m_context.Notify(editor::NoticeKind::Success, Format(u8"Screenshot saved: {}", shots[0].path.AsView()).AsView());
            else
                m_context.Notify(editor::NoticeKind::Error, u8"The screenshot failed (the Console, category Screenshot, says why)");
        }
        const bool screenshotRequested = !m_config.screenshotPath.IsEmpty();
        if (m_config.autoExitSeconds <= 0.0f && m_config.autoRebuildSeconds <= 0.0f && !screenshotRequested)
            return;
        m_elapsed += dt;
        if (m_config.autoExitSeconds > 0.0f && m_elapsed >= m_config.autoExitSeconds)
            host.Shell()->RequestExit();
        if (m_config.autoRebuildSeconds > 0.0f && !m_autoRebuilt && m_elapsed >= m_config.autoRebuildSeconds)
        {
            m_autoRebuilt = true;
            m_cookService.RequestCook(true);
        }
        if (screenshotRequested && !m_screenshotFired && m_elapsed >= m_config.screenshotAfterSeconds)
        {
            m_screenshotFired = true; // one shot: the next main-window frame records it
            if (graphics::RenderWindow* main = host.MainRenderWindow(); main != nullptr)
            {
                WindowShotRequest request;
                request.window = main->Window().Id();
                request.main = true;
                request.path = m_config.screenshotPath;
                m_windowCapture.Request(Span<const WindowShotRequest>(&request, 1));
            }
        }
        // The capture settles a frame or two after it fires; Busy clears whether it was written or failed.
        if (screenshotRequested && m_config.screenshotExit && m_screenshotFired && !m_windowCapture.Busy())
            host.Shell()->RequestExit();
    }

    void EditorApplication::OnRenderWindow(runtime::IApplicationHost& host, graphics::FrameContext& frame)
    {
        // Every page's viewport content renders in the MAIN window's frame, inside one
        // scene-renderer bracket, before the GUI draws: floats' windows sample what it made.
        if (frame.valid && frame.window == host.MainRenderWindow())
        {
            if (m_sceneRenderer != nullptr)
                m_sceneRenderer->BeginRendering(*frame.encoder, frame.frameIndex);
            m_pages->Render(host, frame);
            if (m_sceneRenderer != nullptr)
                m_sceneRenderer->EndRendering();
            m_pages->AfterSceneRender(host, frame); // overlays on the composed targets (the Game tab's UI)
            if (m_embeddedApp)
                m_runtimeContext.EndFrame();
        }
        m_window.Render(frame);
        // A window screenshot: this window's finished backbuffer, the GUI included, when one is armed;
        // OnUpdate writes it next frame.
        if (frame.valid && frame.window != nullptr && frame.encoder != nullptr && host.Graphics() != nullptr && host.Graphics()->Raw() != nullptr)
            m_windowCapture.Record(*host.Graphics()->Raw(), *frame.encoder, frame.window->Window().Id(), frame.backbuffer,
                                   frame.window->Swap()->Format(), frame.width, frame.height);
    }

    Array<u32> EditorApplication::CapturableWindows()
    {
        Array<u32> windows(m_editorAllocator);
        if (m_host == nullptr)
            return windows;
        graphics::RenderWindow* main = m_host->MainRenderWindow();
        if (main == nullptr || !main->Window().IsOpen() || main->Window().IsMinimized())
            return windows; // a minimised editor draws no frame
        windows.PushBack(main->Window().Id());
        if (m_host->Shell() != nullptr && m_host->Shell()->WindowManager() != nullptr)
            for (foundation::shell::IWindow* window : m_host->Shell()->WindowManager()->Windows())
                if (window != nullptr && window->Id() != main->Window().Id() && window->IsOpen() && !window->IsMinimized())
                    windows.PushBack(window->Id());
        return windows;
    }

    void EditorApplication::TakeEditorScreenshot()
    {
        const Array<u32> windows = CapturableWindows();
        if (windows.IsEmpty())
            return;
        WindowShotRequest request;
        request.window = windows[0];
        request.main = true;
        request.path = NewScreenshotPath(u8"editor");
        if (request.path.IsEmpty())
        {
            m_context.Notify(editor::NoticeKind::Error, u8"Could not create the screenshots folder");
            return;
        }
        m_windowCapture.Request(Span<const WindowShotRequest>(&request, 1));
        m_announceScreenshot = true;
    }

    void EditorApplication::OnShutdown(runtime::IApplicationHost& host)
    {
        if (auto* gfx = host.Graphics(); gfx != nullptr && gfx->Raw() != nullptr)
            m_windowCapture.Release(*gfx->Raw()); // the readback buffers, while the device lives
        if (host.Shell() != nullptr)
            host.Shell()->OnMainWindowCloseRequested = {};
        m_session.Close(); // the MCP host, the cook, the native module and the resources, in order
        m_mcp.Reset();
        m_cookService.Shutdown(); // joins a cook in flight
        if (m_embeddedApp)
        {
            m_embeddedApp->OnShutdown(*m_embeddedHost);
            m_runtimeContext.Shutdown();
        }
        m_preferences.Reset();
        m_palette.Reset();
        m_menus.Reset();
        m_shortcuts.Reset();
        m_dock.Close(); // its floats' windows, then the GUI before the device goes
        m_window.Close();
    }
}
