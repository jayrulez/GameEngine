// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Project - :tool_plugins partition.
//
// The plugins of a headless tool (the cook, the exporter, the MCP host): a tool has no
// application, so its plugin host runs over a subsystem manager of its own. Made once the
// project is open, it loads the plugins the project enables that the tool links, each the parts
// the tool's role loads, a Pipeline part adding to the tool's pipeline registries, and unloads
// them when it goes, before the project it read them from.

module;
#include "Core/Prelude.h"

export module editor.project:tool_plugins;

import foundation.core;
import foundation.runtime;
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
            : m_subsystems(allocator), m_pipeline(allocator, registries),
              m_host(allocator, m_subsystems, role, engine::project::kEngineVersionString)
        {
            (void)m_pipeline.Attach(m_host);
            if (attach)
            {
                attach(m_host);
            }
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
        pipeline::PipelinePluginServices m_pipeline; // what the Pipeline parts add, reversed as they unload
        foundation::runtime::PluginHost m_host;      // after what it runs over and records through, so it goes first
        foundation::runtime::PluginLoadReport m_report;
    };
}
