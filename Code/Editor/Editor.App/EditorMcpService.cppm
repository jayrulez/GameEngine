// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::App - :mcp_service partition.
//
// EditorMcpService: the editor's MCP host over the open project, out of the application
// (Documentation/Specs/editor-gui.md, 5.1, layer 3). It starts when a project opens (when the
// user enabled it, or --mcp asked for it), minting the token the first time, with the shared
// engine tools over the application's own services (an agent's cook, import or export takes the
// paths the menus do) and the editor host's live additions: the pages, the windows' screenshots,
// the profiler. The application adds the tools that need its other parts (the dock's layout
// tools, the action bridge) through `addTools`. It stops before the project's services go.
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
import :mcp_host;
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
            editor::EditorJobService* jobs = nullptr;
            pipeline::BuilderRegistry* builders = nullptr;
            foundation::settings::Settings* userSettings = nullptr; // EditorMcpSettings: enabled, port, token
            editor::EditorLogBuffer* log = nullptr;                 // what log_read serves
            String dataRoot;
            /// The command line's: on whatever the preference says, and a port of its own (0: the preference's).
            bool forceEnabled = false;
            u32 port = 0;
            /// The templates' folder the export operations use (read on the main thread).
            Function<String()> templatesRoot;
            /// What an agent's create, delete and import do beside the store (the assets list).
            Function<void(const pipeline::AssetCreator&, foundation::content::Instance&)> created;
            Function<bool(const Guid&)> deleteAsset;
            Function<void(foundation::content::Instance&, const pipeline::ImportOptions*)> imported;
            /// The pages, over the paths the tabs take.
            Function<editor::EditorPage*(const Guid&)> openPage;
            Function<void(editor::EditorPage*)> closePage;
            /// The windows' screenshots (the application's capture).
            WindowToolSeams windows;
            /// The main window's GUI, which the input tools drive.
            Function<foundation::gui::SceneRoot*()> guiRoot;
            /// The tools the application adds over its other parts (the dock, the actions).
            Function<void(foundation::mcp::McpServer&)> addTools;
        };

        EditorMcpService(IAllocator& allocator, Services services) : m_allocator(allocator), m_services(Move(services)) {}
        EditorMcpService(const EditorMcpService&) = delete;
        EditorMcpService& operator=(const EditorMcpService&) = delete;
        ~EditorMcpService() { Stop(); }

        /// Starts the host over `project` when enabled: false when it is not, or it could not
        /// listen (logged: another editor may hold the port).
        bool Start(editor::EditorProject& project)
        {
            Stop();
            editor::EditorMcpSettings& settings = m_services.userSettings->Section<editor::EditorMcpSettings>();
            if (!(settings.enabled || m_services.forceEnabled))
                return false;
            if (m_services.log == nullptr)
            {
                LOG_WARNING(u8"Editor", u8"MCP: no log capture was installed - the host is not started");
                return false;
            }
            if (settings.token.IsEmpty())
            {
                // The first enable mints the secret once and keeps it: the token file and the
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
            m_session.project = &project;

            EditorProjectOperationsSeams operations;
            operations.allocator = &m_allocator;
            operations.project = &project;
            operations.context = m_services.context;
            operations.cook = m_services.cook;
            operations.jobs = m_services.jobs;
            operations.builders = m_services.builders;
            operations.hostToolDir = GetExecutableDirectory();
            operations.templatesRoot = m_services.templatesRoot ? m_services.templatesRoot() : String{};
            operations.dataRoot = m_services.dataRoot;
            // Forwarded, not moved: the host starts again for every project.
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
            editor::EditorContext* context = m_services.context;
            m_host->OnToolFinished = [context](StringView tool, bool isError)
            { context->SetStatus(Format(u8"MCP: {} {}", tool, isError ? u8"failed" : u8"done")); };

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
            RegisterInputTools(m_host->Server(), Move(input));
            ProfilerToolSeams profiler;
            profiler.profiler = &foundation::profiler::Profiler::Get();
            profiler.recorder = &foundation::profiler::CaptureRecorder::Get();
            profiler.recorder->SetApplication(u8"Editor"); // its captures' names and header
            profiler.folder = []() { return PathJoin(GetUserDataDirectory().AsView(), u8"profiles"); };
            RegisterProfilerTools(m_host->Server(), Move(profiler));
            if (m_services.addTools)
                m_services.addTools(m_host->Server());

            EditorMcpHostConfig config;
            config.port = static_cast<u16>(m_services.port != 0 ? m_services.port : settings.port);
            config.token = settings.token;
            config.tokenFileDirectory = GetUserDataDirectory();
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
            return true;
        }

        /// Stops the host: a waiting agent sees its connection close.
        void Stop()
        {
            m_host.Reset();
            m_operations.Reset();
            m_session.project = nullptr;
        }

        /// Each frame: the calls that arrived are answered on the main thread.
        void Pump()
        {
            if (m_host)
                m_host->Pump();
        }

        [[nodiscard]] bool IsRunning() const noexcept { return m_host.Get() != nullptr; }
        [[nodiscard]] EditorMcpHost* Host() const noexcept { return m_host.Get(); }

    private:
        IAllocator& m_allocator;
        Services m_services;
        editor::mcp::ProjectSession m_session;
        UniquePtr<EditorProjectOperations> m_operations;
        UniquePtr<EditorMcpHost> m_host; // after the operations, gone before them
    };
}
