// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell
// Editor::App - :mcp_input_tools partition.
//
// Input tools over MCP: editor_find, editor_click, editor_hover, editor_scroll, editor_type and
// editor_key drive the editor's own GUI as a user would, so an agent can check what a page does (a
// row chosen, a group opened, a key captured) and then look with editor_screenshot. The input goes
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
    };

    /// The number of tools RegisterInputTools registers (editor_find, editor_click, editor_hover,
    /// editor_scroll, editor_type, editor_key); a tripwire like kPageToolCount.
    inline constexpr usize kInputToolCount = 6;

    void RegisterInputTools(foundation::mcp::McpServer& server, InputToolSeams seams);
}
