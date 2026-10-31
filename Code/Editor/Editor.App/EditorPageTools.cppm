// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell
// Editor::App - :mcp_page_tools partition.
//
// The MCP tools over the editor's open pages, which only the editor host can serve: page_list,
// page_open, page_reload and page_close, over the context's page list and the two actions the
// application owns (opening a source asset's page, closing a page with its panel). Reload and
// close are refused while the page has unsaved changes unless the agent says so explicitly
// (`force`, `discard`): a tool never waits for a human, so the destructive decision is an
// argument, and the refusal tells the agent to ask the user.
module;
#include "Core/Prelude.h"

export module editor.app:mcp_page_tools;

import foundation.core;
import foundation.mcp;
import editor.core;

using namespace foundation::core;

export namespace editor::app
{
    /// The live editor the page tools see. The context lists the pages; the application
    /// supplies the two actions that involve a page's panel, which the context does not own.
    struct PageToolSeams
    {
        editor::EditorContext* context = nullptr;
        /// Opens (or focuses) the page for a source asset. Null = no such asset, or no page for
        /// its type; the reason is in the log.
        Function<editor::EditorPage*(const Guid&)> openPage;
        /// Closes a page for good, panel and all.
        Function<void(editor::EditorPage*)> closePage;
    };

    /// The number of tools RegisterPageTools registers (page_list, page_open, page_reload and
    /// page_close); a tripwire like kEngineToolCount.
    inline constexpr usize kPageToolCount = 4;

    void RegisterPageTools(foundation::mcp::McpServer& server, PageToolSeams seams);
}
