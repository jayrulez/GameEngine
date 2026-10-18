// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Render - the `editor.render` module.
//
// The render domain's editor page: SettingsProfilePage (an environment or a post-process
// profile), its factory and RegisterSettingsProfileEditor, which the editor EXECUTABLE calls. It
// builds on editor.scene: its rows are a scene's settings rows, and it previews on the project's
// scenes.

export module editor.render;

export import :settings_profile_page;
