// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Play - :registration partition.
//
// RegisterPlayInEditor: the module's entry point, which the editor EXECUTABLE calls beside
// RegisterSceneEditor. It gives the context its Game tab factory (Play and Play New Instance
// open one) and contributes the PIE tools to the editor's MCP host.

module;
#include "Core/Prelude.h"

export module editor.play:registration;

import foundation.core;
import foundation.runtime;
import foundation.ui.runtime;
import foundation.mcp;
import engine.defaultapp;
import engine.gameinstance; // GameInstance (the Game tab's run; multi-instance factory)
import editor.core;
import :game_page;
import :pie_tools;

using namespace foundation::core;

export namespace editor
{
    inline void RegisterPlayInEditor(EditorContext& context, foundation::runtime::IApplicationHost& host,
                                     foundation::ui::runtime::UIHost& uiHost,
                                     engine::runtime::DefaultApplication* embeddedApp)
    {
        EditorContext* editorContext = &context;
        foundation::runtime::IApplicationHost* appHost = &host;

        // The Game tab (player behavior in-process): the primary, or another instance's.
        context.GamePageFactory = [editorContext, appHost, appUiHost = &uiHost, embeddedApp](
                                      bool newInstance, StringView pieId) -> UniquePtr<EditorPage>
        {
            // Reuse the primary instance for the normal Play; spin up an extra for "Play New Instance".
            engine::runtime::GameInstance* instance =
                newInstance ? embeddedApp->CreateInstance() : &embeddedApp->Instance();
            return UniquePtr<EditorPage>(
                editor::EditorRootAllocator().New<GameEditorPage>(*editorContext, *appHost, *appUiHost,
                                                                  embeddedApp, instance, pieId),
                editor::EditorRootAllocator());
        };

        // Play in editor for agents: start, stop, state and capture, per Game tab.
        context.RegisterMcpToolContribution([editorContext](foundation::mcp::McpServer& server)
                                            { RegisterPieTools(server, *editorContext); });
    }
}
