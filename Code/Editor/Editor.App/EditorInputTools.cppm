// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell
// Editor::App - :mcp_input_tools partition.
//
// Input tools over MCP: editor_find, editor_click, editor_hover, editor_scroll, editor_type,
// editor_key and editor_drop drive the editor's own GUI as a user would, so an agent can check what
// a page does (a row chosen, a group opened, a key captured, files dropped for import) and then look
// with editor_screenshot. editor_drop hands its files to what the window does with a file dropped
// from the operating system, so the import it opens is the one a user's drop opens. The input goes
// to the editor's GUI dispatcher, not the operating system, so it reaches only this editor, even
// with its window hidden or the user working beside it. A target is named as the GUI's tests name
// one (GUI.Testing's Finder: a CSS selector, "text=Jump", "text~=Jum") or by a point in the
// screenshot's pixels. A click aims at the middle of what it names and is refused when something
// else lies there. Each acting tool answers a frame later, so what it reports (the focus, what the
// pointer is over) is what the following frame shows. Only the main window is reached; a floating
// window's GUI is not.
module;
#include "Core/Prelude.h"

export module editor.app:mcp_input_tools;

import foundation.core;
import foundation.gui;
import foundation.mcp;

using namespace foundation::core;

export namespace editor::app
{
    /// The live editor the input tools drive: the main window's GUI (null when there is none; the
    /// tools then report that).
    struct InputToolSeams
    {
        Function<foundation::gui::SceneRoot*()> root;
        /// Drops files on the main window as the operating system would; false with `error` when
        /// they cannot be taken (no project open).
        Function<bool(Span<const String> paths, String& error)> drop;
    };

    /// The number of tools RegisterInputTools registers (editor_find, editor_click, editor_hover,
    /// editor_scroll, editor_type, editor_key, editor_drop); a tripwire like kPageToolCount.
    inline constexpr usize kInputToolCount = 7;

    void RegisterInputTools(foundation::mcp::McpServer& server, InputToolSeams seams);
}
