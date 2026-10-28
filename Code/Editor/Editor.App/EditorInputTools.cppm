// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell
// Editor::App - :mcp_input_tools partition.
//
// The user's hands over MCP: editor_find, editor_click, editor_hover, editor_scroll, editor_type
// and editor_key drive the editor's own GUI as a user would, so an agent checks what a page does
// (a row chosen, a group opened, a key captured) and then looks with editor_screenshot. The input
// goes to the editor's GUI dispatcher, never the operating system: it reaches only this editor,
// with its window hidden or the user at work beside it. A target is named as the GUI's tests name
// one (GUI.Testing's Finder: a CSS selector, "text=Jump", "text~=Jum"), or by a point in the
// screenshot's pixels; a click aims at the middle of what it names, and is refused when something
// else lies there. Each acting tool answers a frame later, so what it reports (the focus, what the
// pointer is over) is what the frame after it shows. The main window only: a floating window's
// GUI is not reached.
module;
#include "Core/Prelude.h"

export module editor.app:mcp_input_tools;

import foundation.core;
import foundation.gui;
import foundation.mcp;

using namespace foundation::core;

export namespace editor::app
{
    /// The live editor the input tools drive: the main window's GUI (null: none now, the tools
    /// say so).
    struct InputToolSeams
    {
        Function<foundation::gui::SceneRoot*()> root;
    };

    /// The number of tools RegisterInputTools registers (editor_find, editor_click, editor_hover,
    /// editor_scroll, editor_type, editor_key); a tripwire like kPageToolCount.
    inline constexpr usize kInputToolCount = 6;

    void RegisterInputTools(foundation::mcp::McpServer& server, InputToolSeams seams);
}
