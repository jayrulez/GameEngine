// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell
// Editor::Play - :pie_tools partition.
//
// The play-in-editor tools the editor's MCP host serves through the scene editor's tool
// contribution: pie_start, pie_stop, pie_state, pie_list, pie_resolution, pie_screenshot and
// pie_run (the playtest, in PieRunToolImpl.cpp). Play in editor is not one game: every Game tab
// runs its own instance, so every call addresses one instance by its `pie` id (`game-page` for
// the primary, `game-page-1`, ... for Play New Instance's), defaulting to the primary, and every
// answer names the instance it is about. A tab is reached through the interface it publishes
// (EditorPage::Service<IPieInstancePage>).
module;
#include "Core/Prelude.h"

export module editor.play:pie_tools;

import foundation.core;
import foundation.json;
import foundation.mcp;
import editor.core;
import :pie_page_interface;

using namespace foundation::core;

export namespace editor
{
    /// The number of tools RegisterPieTools registers; a tripwire like kSceneLiveToolCount.
    inline constexpr usize kPieToolCount = 7;

    void RegisterPieTools(foundation::mcp::McpServer& server, EditorContext& context);

    /// The Game tab a call addresses (its `pie` argument, or the primary), or the reason for the
    /// agent to read when no such tab is open. The page publishes IPieInstancePage.
    [[nodiscard]] Result<EditorPage*, String> ResolvePie(const EditorContext& context,
                                                         const foundation::json::JsonValue& args);
}

namespace editor
{
    // Internal to the module: pie_run's registration (PieRunToolImpl.cpp), and the state pie_state
    // answers with, which a finished run reports too.
    void RegisterPieRunTool(foundation::mcp::McpServer& server, EditorContext& context);
    [[nodiscard]] foundation::json::JsonValue PieStateJson(const IPieInstancePage& pie);
    // Writes a capture's size into a tool's answer: the PNG's `width` and `height` (the pixels the
    // tab drew), and the game's render resolution and the scale it was drawn at when they differ.
    void WriteCaptureSize(foundation::json::JsonValue& out, const ViewportCapture& capture);
}
