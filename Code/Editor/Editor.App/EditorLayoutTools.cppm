// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell
// Editor::App - :mcp_layout_tools partition.
//
// The MCP tools over the editor's dock layout (GUI.Docking's, Documentation/Specs/editor-gui.md,
// 5.1, layer 14) - what only the editor host can serve: layout_get (the tree of splits, each with
// any number of children and their weights (ruling 16), and stacks of tabs, every panel by its id
// and title, the floating and hidden ones) and panel_dock (a panel moved beside another, into its
// tabs, along the window's edge, or floating), so an agent can put two Game tabs side by side for a
// networked playtest as a drag would. A panel is named by its id: a page's asset guid, a Game
// tab's PIE id (`game-page`, `game-page-1`, ...), or a shared panel's (`welcome` for Project,
// `assets`, `console`, `profiler`).
module;
#include "Core/Prelude.h"

export module editor.app:mcp_layout_tools;

import foundation.core;
import foundation.mcp;
import foundation.gui.docking;

using namespace foundation::core;

export namespace editor::app
{
    /// The number of tools RegisterLayoutTools registers (layout_get / panel_dock); a tripwire
    /// like kPageToolCount.
    inline constexpr usize kLayoutToolCount = 2;

    /// Registers the layout tools over `dock`, the editor's dock controller (it outlives the
    /// server).
    void RegisterLayoutTools(foundation::mcp::McpServer& server, foundation::gui::DockController& dock);
}
