// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Mcp - :plugin_services partition
//
// What a plugin's Tooling part adds over MCP, through the service its host provides
// (PluginContext::Find<McpPluginServices>): tools, which every MCP server the service serves
// offers, one there before the plugin loaded and one started after alike. The service keeps each
// plugin's tool registrations, applies them to a server as it is served, and takes the tools out
// of every server when their plugin unloads. A plugin's tool may not take a name already served:
// the engine's tools and other plugins' keep theirs.

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"

export module editor.mcp:plugin_services;

import foundation.core;
import foundation.json;
import foundation.mcp;
import foundation.runtime;
import pipeline.importer; // PipelineRegistries
import editor.project;    // ExportPluginServices
import :session;

using namespace foundation::core;
using foundation::json::JsonValue;

export namespace editor::mcp
{
    /// Where a Tooling part registers its tools: the server being served and the session its tools
    /// work through.
    class PluginTools
    {
    public:
        PluginTools(foundation::mcp::McpServer& server, ProjectSession& session, Array<String>& registered)
            : m_server(&server), m_session(&session), m_registered(&registered)
        {
        }

        [[nodiscard]] ProjectSession& Session() noexcept { return *m_session; }

        /// Registers a tool; false (nothing registered, a warning logged) when the name is served.
        bool RegisterTool(String name, String description, JsonValue schema, foundation::mcp::ToolAnnotations annotations,
                          foundation::mcp::ToolHandler handler)
        {
            if (!Claim(name.AsView()))
            {
                return false;
            }
            m_registered->PushBack(name);
            m_server->RegisterTool(Move(name), Move(description), Move(schema), annotations, Move(handler));
            return true;
        }
        /// A tool whose handler sees its call (one answering NotFinished), as McpServer takes it.
        bool RegisterTool(String name, String description, JsonValue schema, foundation::mcp::ToolAnnotations annotations,
                          foundation::mcp::ToolCallHandler handler)
        {
            if (!Claim(name.AsView()))
            {
                return false;
            }
            m_registered->PushBack(name);
            m_server->RegisterTool(Move(name), Move(description), Move(schema), annotations, Move(handler));
            return true;
        }

    private:
        [[nodiscard]] bool Claim(StringView name) const
        {
            if (!m_server->HasTool(name))
            {
                return true;
            }
            LOG_WARNING(u8"Plugins", u8"a plugin's MCP tool '{}' is not served: the name is taken", name);
            return false;
        }

        foundation::mcp::McpServer* m_server;
        ProjectSession* m_session;
        Array<String>* m_registered;
    };

    class McpPluginServices final : public foundation::runtime::IRegistrationRecorder
    {
    public:
        /// Registers a plugin's tools into one server.
        using ToolRegistrar = Function<void(PluginTools&)>;

        explicit McpPluginServices(IAllocator& allocator)
            : m_allocator(&allocator), m_contributions(allocator), m_servers(allocator)
        {
        }
        ~McpPluginServices() override = default;

        McpPluginServices(const McpPluginServices&) = delete;
        McpPluginServices& operator=(const McpPluginServices&) = delete;

        /// Hands this service to `host`'s plugins and records what they add through it. Before the
        /// host loads; this outlives the host.
        [[nodiscard]] bool Attach(foundation::runtime::PluginHost& host)
        {
            if (!host.Provide(*this))
            {
                return false;
            }
            host.Loader().AddRecorder(this);
            return true;
        }

        /// Serves `server` (its tools working through `session`, both borrowed until DetachServer):
        /// every loaded plugin's tools join it now, and a plugin loaded later adds its own.
        void AttachServer(foundation::mcp::McpServer& server, ProjectSession& session)
        {
            if (FindServer(server) != nullptr)
            {
                return;
            }
            m_servers.PushBack(Served{&server, &session});
            for (Contribution& contribution : m_contributions)
            {
                Apply(contribution, m_servers.Back());
            }
        }

        /// Stops serving `server`: the plugins' tools leave it (a server going away calls this first).
        void DetachServer(foundation::mcp::McpServer& server)
        {
            for (Contribution& contribution : m_contributions)
            {
                Withdraw(contribution, server);
            }
            for (usize i = 0; i < m_servers.Size(); ++i)
            {
                if (m_servers[i].server == &server)
                {
                    m_servers.RemoveAt(i);
                    return;
                }
            }
        }

        /// Adds the tools `registrar` registers to every server this service serves, and to each it
        /// serves later; they leave with the plugin. False for an empty registrar.
        bool AddTools(ToolRegistrar registrar)
        {
            if (!registrar)
            {
                return false;
            }
            const u64 token = m_nextToken++;
            m_contributions.PushBack(Contribution{token, Move(registrar), Array<Applied>(*m_allocator)});
            for (const Served& served : m_servers)
            {
                Apply(m_contributions.Back(), served);
            }
            if (m_sink != nullptr)
            {
                m_sink->PushBack(token);
            }
            return true;
        }

        /// The tools the plugins added to `server`, by name.
        [[nodiscard]] Array<String> ToolsServedBy(const foundation::mcp::McpServer& server) const
        {
            Array<String> names;
            for (const Contribution& contribution : m_contributions)
            {
                for (const Applied& applied : contribution.applied)
                {
                    if (applied.server == &server)
                    {
                        for (const String& name : applied.names)
                        {
                            names.PushBack(name);
                        }
                    }
                }
            }
            return names;
        }

        // IRegistrationRecorder: armed around each part's load, so what it added is recorded
        // against it, and reversed when it unloads.
        void Arm(Array<TypeId>& sink) override { m_sink = &sink; }
        void Disarm() override { m_sink = nullptr; }
        void Reverse(TypeId id) override
        {
            for (usize i = 0; i < m_contributions.Size(); ++i)
            {
                if (m_contributions[i].token != id)
                {
                    continue;
                }
                for (const Served& served : m_servers)
                {
                    Withdraw(m_contributions[i], *served.server);
                }
                m_contributions.RemoveAt(i);
                return;
            }
        }

    private:
        struct Served
        {
            foundation::mcp::McpServer* server;
            ProjectSession* session;
        };
        struct Applied
        {
            foundation::mcp::McpServer* server;
            Array<String> names;
        };
        struct Contribution
        {
            u64 token; // this service's, what the recording keeps
            ToolRegistrar registrar;
            Array<Applied> applied;
        };

        [[nodiscard]] const Served* FindServer(const foundation::mcp::McpServer& server) const noexcept
        {
            for (const Served& served : m_servers)
            {
                if (served.server == &server)
                {
                    return &served;
                }
            }
            return nullptr;
        }

        void Apply(Contribution& contribution, const Served& served)
        {
            Applied applied{served.server, Array<String>(*m_allocator)};
            PluginTools tools(*served.server, *served.session, applied.names);
            contribution.registrar(tools);
            contribution.applied.PushBack(Move(applied));
        }

        static void Withdraw(Contribution& contribution, foundation::mcp::McpServer& server)
        {
            for (usize i = 0; i < contribution.applied.Size(); ++i)
            {
                if (contribution.applied[i].server != &server)
                {
                    continue;
                }
                for (const String& name : contribution.applied[i].names)
                {
                    (void)server.UnregisterTool(name.AsView());
                }
                contribution.applied.RemoveAt(i);
                return;
            }
        }

        IAllocator* m_allocator;
        Array<Contribution> m_contributions;
        Array<Served> m_servers;
        Array<TypeId>* m_sink = nullptr;
        u64 m_nextToken = 1;
    };

    /// What a host that opens projects hands each project's plugins: the plugins it links, the
    /// pipeline registries their Pipeline parts add to, and the service their Tooling parts add MCP
    /// tools through (null: none).
    struct ProjectPlugins
    {
        Span<const foundation::runtime::LinkedPlugin> linked;
        pipeline::PipelineRegistries registries;
        McpPluginServices* tools = nullptr;
        editor::ExportPluginServices* exportHooks = nullptr; // what their Tooling parts run after an export's staging
    };
}
