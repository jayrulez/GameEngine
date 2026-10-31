// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell
// Editor::App - :mcp_host partition.
//
// EditorMcpHost: the editor's MCP host. It serves the engine surface every host serves
// (editor.mcp's RegisterEngineTools, pointed at the editor's live project, so there is one
// ContentDatabase and one writer) plus this host's own host_info, over localhost HTTP and SSE
// behind a bearer token (foundation.mcp.http). It is pumped once per frame on the main thread, so
// a tool may touch anything the editor owns. It lives for one open project: the application
// starts it once the project's services are up and stops it before they go away. The token is
// also written to a file beside the editor's settings, so a local agent can configure itself.
module;
#include "Core/Prelude.h"

export module editor.app:mcp_host;

import foundation.core;
import foundation.mcp;
import foundation.mcp.http;
import pipeline.core;
import editor.core;
import editor.mcp;
import pipeline.importer; // the import framework (a consumer imports it itself)

using namespace foundation::core;

export namespace editor::app
{
    struct EditorMcpHostConfig
    {
        u16 port = 0;              ///< 0 = OS-assigned (tests); the preference's port otherwise
        String token;              ///< the bearer secret; the transport refuses to serve without one
        String tokenFileDirectory; ///< where `mcp-token` is written for a local agent; empty = nowhere
    };

    class EditorMcpHost
    {
    public:
        /// The token file's name under EditorMcpHostConfig::tokenFileDirectory.
        static constexpr ENGINE_EXPORT_DATA StringView kTokenFileName = u8"mcp-token";

        /// Registers the shared engine surface over `session` (pointed at the editor's live
        /// project), this host's host_info, and every domain's MCP tool contribution on
        /// `context` (what the scene editor and the like serve over the live editor);
        /// `operations` is how this host runs the cook, import and export behind their tools.
        /// Every reference must outlive the host: the application owns them all for the
        /// project's life.
        EditorMcpHost(IAllocator& allocator, editor::EditorContext& context,
                      editor::mcp::ProjectSession& session, editor::EditorLogBuffer& logBuffer,
                      pipeline::BuilderRegistry& builders, pipeline::ImporterRegistry& importers,
                      const pipeline::AssetCreatorRegistry& creators,
                      const editor::mcp::EngineToolPaths& paths,
                      editor::mcp::IProjectOperations& operations, String buildStamp);
        /// Stops, and removes what the host wired into the session: the session outlives the
        /// host, so its callbacks must not point at the host afterwards.
        ~EditorMcpHost()
        {
            Stop();
            m_session->onAssetWritten = nullptr;
            m_session->onSettingsChanged = nullptr;
        }
        EditorMcpHost(const EditorMcpHost&) = delete;
        EditorMcpHost& operator=(const EditorMcpHost&) = delete;

        /// Binds the port and writes the token file. False = the port is taken (another editor,
        /// most likely) or the token is empty; the host then serves nothing.
        [[nodiscard]] bool Start(const EditorMcpHostConfig& config);
        void Stop() { m_http.Stop(); }

        /// One pump of the transport: answers a waiting call and re-enters an unfinished one.
        /// Call once per frame, on the main thread.
        void Pump() { (void)m_http.Pump(); }

        [[nodiscard]] bool IsRunning() const noexcept { return m_http.IsRunning(); }
        [[nodiscard]] u16 BoundPort() const noexcept { return m_http.BoundPort(); }
        /// Whether an agent is waiting on an unfinished tool, so pumping must continue.
        [[nodiscard]] bool HasPendingRequest() const noexcept { return m_http.HasPendingRequest(); }
        [[nodiscard]] foundation::mcp::McpServer& Server() noexcept { return m_server; }

        /// Called with (tool, isError) after every finished tools/call, so the application can
        /// show the agent's activity.
        Function<void(StringView, bool)> OnToolFinished;

    private:
        IAllocator* m_allocator;
        foundation::mcp::McpServer m_server;
        foundation::mcp::McpHttpHost m_http;
        editor::mcp::ProjectSession* m_session;
    };
}
