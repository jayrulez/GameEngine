// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::App - :mcp_service partition.
//
// EditorMcpService: the editor's MCP host over the open project, kept out of the application. It
// starts when a project opens (when the user enabled it, or --mcp asked for it), creating the token
// the first time, and while the project manager shows it serves with no project: the engine's tools
// refuse as they do before project_open, and the input, screenshot, layout and action tools drive
// the manager. It serves the shared engine tools over the application's own services, so an
// agent's cook, import or export takes the same paths the menus do, plus the editor host's live
// additions: the pages, the windows' screenshots, the profiler. The application adds the tools that
// need its other parts (the dock's layout tools, the action bridge) through `addTools`. It stops
// before the project's services go away. The user starts and stops it from the MCP panel too: their
// Start runs it whatever the preference says, and their Stop holds until they start it again or
// change the MCP preferences, across the projects opened between. What agents did is kept in its
// activity (McpActivity), which outlives the host's restarts.
module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"

export module editor.app:mcp_service;

import foundation.core;
import foundation.content;
import foundation.mcp;
import foundation.gui; // the input tools' root
import foundation.vfs;
import foundation.shaders.system; // kShaderFolder
import foundation.profiler;
import foundation.profiler.capture;
import foundation.settings;
import pipeline.core;
import pipeline.importer;
import editor.core;
import editor.mcp;
import foundation.runtime; // PluginHost (the project's plugins the session reports)
import :mcp_host;
import :mcp_activity;
import :mcp_operations;
import :mcp_page_tools;
import :mcp_window_tools;
import :mcp_input_tools;
import :mcp_profiler_tools;

using namespace foundation::core;

// The build identity compiled into Runtime.Client (git short hash and generation time): what
// host_info reports. Resolved at the final link.
extern "C" const char* BuildStamp();

export namespace editor::app
{
    class EditorMcpService
    {
    public:
        struct Services
        {
            editor::EditorContext* context = nullptr;
            editor::EditorCookService* cook = nullptr;
            pipeline::BuilderRegistry* builders = nullptr;
            foundation::settings::Settings* userSettings = nullptr; // EditorMcpSettings: enabled, port, token
            editor::EditorLogBuffer* log = nullptr;                 // what log_read serves
            /// The tools the project's plugins add; the host's server serves them while it runs.
            editor::mcp::McpPluginServices* pluginTools = nullptr;
            /// The plugins the editor links (plugin_list offers those the project does not enable).
            Span<const foundation::runtime::LinkedPlugin> linkedPlugins;
            String dataRoot;
            /// Set from the command line: enabled whatever the preference says, with a port of its own (0: the preference's).
            bool forceEnabled = false;
            u32 port = 0;
            /// The templates' folder the export operations use (read on the main thread).
            Function<String()> templatesRoot;
            /// What an agent's create, delete and import do beyond the store (updating the assets list).
            Function<void(const pipeline::AssetCreator&, foundation::content::Instance&)> created;
            Function<bool(const Guid&)> deleteAsset;
            Function<void(foundation::content::Instance&, const pipeline::ImportOptions*)> imported;
            /// Opens the pages, the way the tabs open them.
            Function<editor::EditorPage*(const Guid&)> openPage;
            Function<void(editor::EditorPage*)> closePage;
            /// The windows' screenshots (the application's capture).
            WindowToolSeams windows;
            /// The main window's GUI, which the input tools drive.
            Function<foundation::gui::SceneRoot*()> guiRoot;
            /// What a file dropped on the main window does (editor_drop); false with `error` when it cannot.
            Function<bool(Span<const String> paths, String& error)> dropFiles;
            /// Where the token file and the profiler's captures go (empty: the user-data folder).
            String userDataDirectory;
            /// What the window shows, for host_info's `screen`: "projectManager" or "editor" (empty:
            /// it follows whether a project is open).
            Function<StringView()> screen;
            /// The tools the application adds over its other parts (the dock, the actions).
            Function<void(foundation::mcp::McpServer&)> addTools;
        };

        EditorMcpService(IAllocator& allocator, Services services) : m_allocator(allocator), m_services(Move(services)), m_activity(allocator) {}
        EditorMcpService(const EditorMcpService&) = delete;
        EditorMcpService& operator=(const EditorMcpService&) = delete;
        ~EditorMcpService() { Stop(); }

        /// Starts the host over `project` when enabled: false when it is not, or it could not
        /// listen (logged: another editor may hold the port).
        bool Start(editor::EditorProject& project) { return Start(&project); }
        /// Starts the host with no project (the project manager's).
        bool StartWithoutProject() { return Start(nullptr); }
        /// The host running, with or without a project: started without one when it is not running.
        /// False when it is disabled or could not listen.
        bool EnsureRunning() { return IsRunning() || StartWithoutProject(); }

        /// Starts the host over `project`, or over none (null), when the preference, the command line
        /// or the user's own Start asks for it and the user's Stop does not hold.
        bool Start(editor::EditorProject* project)
        {
            Stop();
            editor::EditorMcpSettings& settings = m_services.userSettings->Section<editor::EditorMcpSettings>();
            if (m_stopHeld || !(settings.enabled || m_services.forceEnabled || m_userStarted))
                return false;
            if (m_services.log == nullptr)
            {
                LOG_WARNING(u8"Editor", u8"MCP: no log capture was installed - the host is not started");
                return false;
            }
            if (settings.token.IsEmpty())
            {
                // The first enable creates the secret once and keeps it, so the token file and the
                // Preferences display stay valid across runs.
                settings.token = editor::GenerateMcpToken();
                if (!editor::SaveEditorSettingsToUserData(*m_services.userSettings).IsOk())
                    LOG_WARNING(u8"Editor", u8"MCP: the minted token could not be saved to the editor settings; it changes on the next run");
            }
            editor::mcp::EngineToolPaths paths;
            const String starts[] = {GetExecutableDirectory(), GetCurrentDirectory()};
            editor::mcp::LocateShippingDocs(Span<const String>(starts, 2), paths);
            // shader_validate compiles against the engine's shader library under the data root.
            paths.shaderLibrary = foundation::vfs::DataPath(m_services.dataRoot.AsView(), foundation::shaders::kShaderFolder);
            m_session.project = project;
            m_session.plugins = m_services.context->Plugins(); // the project's plugins, loaded with it
            m_session.exportHooks = m_services.context->ExportHooks(); // what project_export runs after staging
            m_session.linkedPlugins = m_services.linkedPlugins;

            EditorProjectOperationsSeams operations;
            operations.allocator = &m_allocator;
            operations.project = project; // null without a project: the tools that use it refuse first
            operations.context = m_services.context;
            operations.cook = m_services.cook;
            operations.tasks = &m_services.context->Tasks(); // each step on the build lane
            operations.lane = m_services.context->BuildLane();
            operations.builders = m_services.builders;
            operations.hostToolDir = GetExecutableDirectory();
            operations.templatesRoot = m_services.templatesRoot ? m_services.templatesRoot() : String{};
            operations.dataRoot = m_services.dataRoot;
            // Forwarded, not moved, because the host starts again for every project.
            EditorMcpService* self = this;
            operations.onCreated = [self](const pipeline::AssetCreator& creator, foundation::content::Instance& instance)
            {
                if (self->m_services.created)
                    self->m_services.created(creator, instance);
            };
            operations.onDelete = [self](const Guid& id) { return self->m_services.deleteAsset && self->m_services.deleteAsset(id); };
            operations.onImported = [self](foundation::content::Instance& primary, const pipeline::ImportOptions* options)
            {
                if (self->m_services.imported)
                    self->m_services.imported(primary, options);
            };
            m_operations = MakeUnique<EditorProjectOperations>(m_allocator, Move(operations));
            m_host = MakeUnique<EditorMcpHost>(m_allocator, m_allocator, *m_services.context, m_session, *m_services.log, *m_services.builders,
                                               m_services.context->Importers(), m_services.context->Creators(), paths, *m_operations,
                                               String(reinterpret_cast<const char8_t*>(BuildStamp())));
            m_host->screen = [self]() { return self->m_services.screen ? self->m_services.screen() : StringView{}; };
            editor::EditorContext* context = m_services.context;
            m_host->OnCall = [self, context](const foundation::mcp::ToolCallReport& report)
            {
                self->m_activity.Record(report, LocalNow());
                if (report.phase == foundation::mcp::ToolCallPhase::Finished)
                    context->SetStatus(Format(u8"MCP: {} {}", report.tool, report.isError ? u8"failed" : u8"done"));
            };

            PageToolSeams pages;
            pages.context = m_services.context;
            pages.openPage = [self](const Guid& id) { return self->m_services.openPage ? self->m_services.openPage(id) : nullptr; };
            pages.closePage = [self](editor::EditorPage* page)
            {
                if (self->m_services.closePage)
                    self->m_services.closePage(page);
            };
            RegisterPageTools(m_host->Server(), Move(pages));
            WindowToolSeams windows;
            windows.windows = [self]() { return self->m_services.windows.windows ? self->m_services.windows.windows() : Array<u32>{}; };
            windows.request = [self](Span<const WindowShotRequest> requests)
            {
                if (self->m_services.windows.request)
                    self->m_services.windows.request(requests);
            };
            windows.shots = [self]() { return self->m_services.windows.shots ? self->m_services.windows.shots() : Array<WindowShot>{}; };
            RegisterWindowTools(m_host->Server(), Move(windows));
            InputToolSeams input;
            input.root = [self]() { return self->m_services.guiRoot ? self->m_services.guiRoot() : nullptr; };
            input.drop = [self](Span<const String> paths, String& error)
            {
                if (!self->m_services.dropFiles)
                    return false;
                return self->m_services.dropFiles(paths, error);
            };
            RegisterInputTools(m_host->Server(), Move(input));
            ProfilerToolSeams profiler;
            profiler.profiler = &foundation::profiler::Profiler::Get();
            profiler.recorder = &foundation::profiler::CaptureRecorder::Get();
            profiler.recorder->SetApplication(u8"Editor"); // names its captures and fills their header
            profiler.folder = [self]() { return PathJoin(self->UserData().AsView(), u8"profiles"); };
            RegisterProfilerTools(m_host->Server(), Move(profiler));
            if (m_services.addTools)
                m_services.addTools(m_host->Server());
            // The project's plugins' tools, last, so none takes a name the editor's own tools hold.
            if (m_services.pluginTools != nullptr)
                m_services.pluginTools->AttachServer(m_host->Server(), m_session);

            EditorMcpHostConfig config;
            config.port = static_cast<u16>(m_services.port != 0 ? m_services.port : settings.port);
            config.token = settings.token;
            config.tokenFileDirectory = UserData();
            if (!m_host->Start(config))
            {
                LOG_WARNING(u8"Editor",
                            u8"MCP: could not listen on 127.0.0.1:{} - another editor may hold the port; pass --mcp-port <n> or change "
                            u8"it in Preferences",
                            config.port);
                Stop();
                return false;
            }
            LOG_INFO(u8"Editor", u8"MCP: listening on 127.0.0.1:{} (the token is in <user-data>/{})", m_host->BoundPort(),
                     EditorMcpHost::kTokenFileName);
            m_services.context->SetStatus(Format(u8"MCP host on 127.0.0.1:{}", m_host->BoundPort()));
            m_activity.RecordEvent(Format(u8"Listening on 127.0.0.1:{}", m_host->BoundPort()).AsView(), LocalNow());
            m_client = {};
            return true;
        }

        /// The user's Start (the MCP panel's): runs the host over `project` whatever the preference
        /// says, until the user stops it. False when it could not listen.
        bool StartByUser(editor::EditorProject* project)
        {
            m_stopHeld = false;
            m_userStarted = true;
            return Start(project);
        }
        /// The user's Stop: the host stops and stays stopped, over every project opened, until the
        /// user starts it again or changes the MCP preferences.
        void StopByUser()
        {
            m_userStarted = false;
            m_stopHeld = true;
            Stop();
        }
        /// The MCP preferences changed: the user's own Start or Stop no longer holds, and the host
        /// follows the preferences again over `project`.
        void Restart(editor::EditorProject* project)
        {
            m_userStarted = false;
            m_stopHeld = false;
            (void)Start(project);
        }
        /// Whether the user's Stop holds the host stopped.
        [[nodiscard]] bool IsStopHeld() const noexcept { return m_stopHeld; }

        /// What agents did through the host in this run.
        [[nodiscard]] McpActivity& Activity() noexcept { return m_activity; }
        [[nodiscard]] const McpActivity& Activity() const noexcept { return m_activity; }
        /// The client that last connected (its name and version; empty before any did).
        [[nodiscard]] const foundation::mcp::McpClientInfo& Client() const noexcept { return m_client; }
        /// The port the host listens on (0: stopped), and the one it starts on.
        [[nodiscard]] u16 Port() const noexcept { return m_host ? m_host->BoundPort() : 0; }
        [[nodiscard]] u32 ConfiguredPort() const
        {
            return m_services.port != 0 ? m_services.port : m_services.userSettings->Section<editor::EditorMcpSettings>().port;
        }
        /// The command that wires an agent (Claude Code) to the running host, with its token; empty
        /// while stopped.
        [[nodiscard]] String ConnectCommand() const
        {
            if (!m_host)
                return {};
            return Format(u8"claude mcp add --transport http {}-editor http://127.0.0.1:{}/mcp --header \"Authorization: Bearer {}\"",
                          StringView(ENGINE_ID), m_host->BoundPort(), m_services.userSettings->Section<editor::EditorMcpSettings>().token);
        }

        /// The session the host's tools work over: the project, its plugins, their hooks.
        [[nodiscard]] const editor::mcp::ProjectSession& Session() const noexcept { return m_session; }

        /// The project's plugin host, which the session's plugin tools report and edit (null: none).
        void SetPlugins(foundation::runtime::PluginHost* plugins) noexcept { m_session.plugins = plugins; }

        /// Stops the host: a waiting agent sees its connection close.
        void Stop()
        {
            if (m_host && m_services.pluginTools != nullptr)
                m_services.pluginTools->DetachServer(m_host->Server()); // its plugins' tools leave before it goes
            if (m_host && m_host->IsRunning())
                m_activity.HostStopped(LocalNow()); // a host that never listened has nothing to stop
            m_host.Reset();
            m_operations.Reset();
            m_session.project = nullptr;
            m_session.plugins = nullptr;
            m_session.exportHooks = nullptr;
        }

        /// Called each frame: answers the calls that arrived, on the main thread.
        void Pump()
        {
            if (!m_host)
                return;
            m_host->Pump();
            // A client that initialized since: recorded once per change of who it is.
            const foundation::mcp::McpClientInfo& client = m_host->Server().Client();
            if (!client.name.IsEmpty() && (client.name != m_client.name || client.version != m_client.version))
            {
                m_client = client;
                m_activity.RecordEvent(client.version.IsEmpty() ? Format(u8"{} connected", client.name).AsView()
                                                                : Format(u8"{} {} connected", client.name, client.version).AsView(),
                                       LocalNow());
            }
        }

        [[nodiscard]] bool IsRunning() const noexcept { return m_host.Get() != nullptr; }
        [[nodiscard]] EditorMcpHost* Host() const noexcept { return m_host.Get(); }

    private:
        /// The local time now, in seconds since 1970 (CivilTimeOf reads it).
        [[nodiscard]] static i64 LocalNow()
        {
            const i64 now = GetUnixTime();
            return now + GetLocalTimeOffset(now);
        }
        [[nodiscard]] String UserData() const
        {
            return m_services.userDataDirectory.IsEmpty() ? GetUserDataDirectory() : m_services.userDataDirectory;
        }

        IAllocator& m_allocator;
        Services m_services;
        editor::mcp::ProjectSession m_session;
        UniquePtr<EditorProjectOperations> m_operations;
        UniquePtr<EditorMcpHost> m_host; // declared after the operations, so it is destroyed before them
        McpActivity m_activity;
        foundation::mcp::McpClientInfo m_client;
        bool m_userStarted = false; // the user's Start runs the host whatever the preference says
        bool m_stopHeld = false;    // the user's Stop holds until their Start or a preference change
    };
}
