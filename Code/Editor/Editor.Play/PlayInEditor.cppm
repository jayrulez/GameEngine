// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Play - :registration partition.
//
// RegisterPlayInEditor: the module's entry point, which the editor EXECUTABLE calls beside
// RegisterSceneEditor. It gives the context its Game tab factory (Play and Play New Instance
// open one, each on the editor's GUI from its markup), the game-audio preference, and
// contributes the PIE tools to the editor's MCP host.

module;
#include "Core/Prelude.h"

export module editor.play:registration;

import foundation.core;
import foundation.runtime;
import foundation.mcp;
import engine.defaultapp;
import engine.gameinstance; // GameInstance (the Game tab's run; multi-instance factory)
import editor.core;
import editor.app; // EditorApplication (its window: the GUI host, the markup)
import :game_page;
import :pie_tools;

using namespace foundation::core;

export namespace editor
{
    inline void RegisterPlayInEditor(EditorContext& context, foundation::runtime::IApplicationHost& host, app::EditorApplication& application)
    {
        EditorContext* editorContext = &context;
        foundation::runtime::IApplicationHost* appHost = &host;
        app::EditorApplication* editor = &application;
        RegisterGameAudioEditorSettings(context);

        // The Game tab (player behaviour in-process): the primary, or another instance's.
        context.GamePageFactory = [editorContext, appHost, editor](bool newInstance, StringView pieId) -> UniquePtr<EditorPage>
        {
            engine::runtime::DefaultApplication* embedded = editor->EmbeddedApplication();
            if (embedded == nullptr)
                return UniquePtr<EditorPage>{};
            // The primary instance for Play; another for Play New Instance.
            engine::runtime::GameInstance* instance = newInstance ? embedded->CreateInstance() : &embedded->Instance();
            GameEditorPage::Services services;
            services.gui = editor->Window().Host();
            app::EditorWindow* window = &editor->Window();
            services.loadMarkup = [window](StringView path) { return window->LoadMarkup(path); };
            return UniquePtr<EditorPage>(editor::EditorRootAllocator().New<GameEditorPage>(*editorContext, *appHost, Move(services), embedded, instance, pieId),
                                         editor::EditorRootAllocator());
        };

        // Play in editor for agents: start, stop, state and capture, per Game tab.
        context.RegisterMcpToolContribution([editorContext](foundation::mcp::McpServer& server) { RegisterPieTools(server, *editorContext); });
    }
}
