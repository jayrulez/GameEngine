// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Project - :tool_plugins partition.
//
// The plugins of a headless tool (the cook, the exporter, the MCP host): a tool has no
// application, so its plugin host runs over a subsystem manager of its own. Made once the
// project is open, it loads the plugins the project enables, those the tool links and the
// project's own packages from their development build (a shared tool loads them; a static one
// refuses them), each the parts the tool's role loads, and unloads them when it goes, before the
// project it read them from. An Engine part is offered the engine's service as in the player and the
// editor, so the scene managers and domains it adds join this tool's scenes and composition (a
// headless MCP host's scene tools then know a plugin's components); a Pipeline part adds to the
// tool's pipeline registries.

module;
#include "Core/Prelude.h"

export module editor.project:tool_plugins;

import foundation.core;
import foundation.runtime;
import engine.composition; // EnginePluginServices
import engine.project;
import pipeline.importer; // PipelinePluginServices
import :project;

using namespace foundation::core;

export namespace editor
{
    class ToolPlugins
    {
    public:
        /// `attach` hands the host what else this tool offers its plugins, before they load.
        ToolPlugins(IAllocator& allocator, foundation::runtime::HostRole role, const EditorProject& project,
                    Span<const foundation::runtime::LinkedPlugin> linked, pipeline::PipelineRegistries registries = {},
                    const Function<void(foundation::runtime::PluginHost&)>& attach = {})
            : m_subsystems(allocator), m_engine(allocator), m_pipeline(allocator, registries),
              m_host(allocator, m_subsystems, role, engine::project::kEngineVersionString)
        {
            (void)m_engine.Attach(m_host);
            (void)m_pipeline.Attach(m_host);
            if (attach)
            {
                attach(m_host);
            }
            engine::project::AddProjectPackages(m_host, project.Directory(), project.Settings(), allocator);
            m_report = engine::project::LoadProjectPlugins(m_host, linked, project.Settings());
        }

        ToolPlugins(const ToolPlugins&) = delete;
        ToolPlugins& operator=(const ToolPlugins&) = delete;
        ~ToolPlugins() { m_host.Unload(); }

        [[nodiscard]] foundation::runtime::PluginHost& Host() noexcept { return m_host; }
        /// What the load did: the plugins loaded, those refused with why, those not enabled here.
        [[nodiscard]] const foundation::runtime::PluginLoadReport& Report() const noexcept { return m_report; }

    private:
        foundation::runtime::SubsystemManager m_subsystems;
        engine::EnginePluginServices m_engine;       // what the Engine parts add, reversed as they unload
        pipeline::PipelinePluginServices m_pipeline; // what the Pipeline parts add, reversed as they unload
        foundation::runtime::PluginHost m_host;      // after what it runs over and records through, so it goes first
        foundation::runtime::PluginLoadReport m_report;
    };
}
