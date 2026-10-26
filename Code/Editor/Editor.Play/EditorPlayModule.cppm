// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Play - the `editor.play` module.
//
// Play in editor: the Game tab (GameEditorPage, a fresh run of the project as the player runs it,
// in-process), the resolutions it plays at, IPieInstancePage (what the PIE tools act through),
// the PIE MCP tools, the game-audio editor settings, and RegisterPlayInEditor, which the editor
// EXECUTABLE calls. It builds on editor.scene: a Game tab captures its viewport as a scene page
// does, and publishes IGameScenePage for the scene tools. On the GUI in parts: the Game tab and
// RegisterPlayInEditor come back with the Game tab's port (editor-gui.md, 5.3, layer 7, part 3).

export module editor.play;

export import :game_resolution;
export import :pie_page_interface;
export import :pie_tools;
