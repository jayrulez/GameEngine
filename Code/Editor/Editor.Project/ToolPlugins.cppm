// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Project - :tool_plugins partition.
//
// The plugins of a headless tool (the cook, the exporter, the MCP host): a tool has no
// application, so its plugin host runs over a subsystem manager of its own. Made once the
// project is open, it loads the plugins the project enables that the tool links, each the parts
// the tool's role loads, and unloads them when it goes, before the project it read them from.

module;
#include "Core/Prelude.h"

export module editor.project:tool_plugins;

import foundation.core;
import foundation.runtime;
import engine.project;
import :project;

using namespace foundation::core;

export namespace editor
{
    class ToolPlugins
    {
    public:
        ToolPlugins(IAllocator& allocator, foundation::runtime::HostRole role, const EditorProject& project,
                    Span<const foundation::runtime::LinkedPlugin> linked)
            : m_subsystems(allocator), m_host(allocator, m_subsystems, role, engine::project::kEngineVersionString)
        {
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
        foundation::runtime::PluginHost m_host; // after the manager it runs over, so it goes first
        foundation::runtime::PluginLoadReport m_report;
    };
}
