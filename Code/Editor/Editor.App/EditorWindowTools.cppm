// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell
// Editor::App - :mcp_window_tools partition.
//
// editor_screenshot: the whole editor window as the user sees it, over MCP, while the editor
// runs - the panels, the pages, the dialogs, the toasts, where viewport_screenshot and
// pie_screenshot show one viewport. Only the editor host can serve it (the windows are the
// application's), so the application hands it seams over its EditorWindowCapture.
module;
#include "Core/Prelude.h"

export module editor.app:mcp_window_tools;

import foundation.core;
import foundation.mcp;
import :window_capture;

using namespace foundation::core;

export namespace editor::app
{
    /// The live editor's windows, as the screenshot tool sees them.
    struct WindowToolSeams
    {
        /// The ids of the editor's windows that can draw a frame now (open, not minimised), the
        /// main window first. Empty = nothing to capture.
        Function<Array<u32>()> windows;
        /// Arm a capture of each.
        Function<void(Span<const WindowShotRequest>)> request;
        /// Where the latest request's captures stand.
        Function<Array<WindowShot>()> shots;
    };

    /// A new PNG path under <user-data>/screenshots for `stem` (the directory made if needed):
    /// <stem>-<pid>-<n>.png, n counting up across every caller in the process (the tool and the
    /// View > Screenshot action never pick one name). Empty when the directory cannot be made.
    [[nodiscard]] String NewScreenshotPath(StringView stem);

    /// The number of tools RegisterWindowTools registers (editor_screenshot); a tripwire like
    /// kPageToolCount.
    inline constexpr usize kWindowToolCount = 1;

    void RegisterWindowTools(foundation::mcp::McpServer& server, WindowToolSeams seams);
}
