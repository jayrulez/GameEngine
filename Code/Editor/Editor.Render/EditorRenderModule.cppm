// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Render - the `editor.render` module.
//
// The render domain's editor page: SettingsProfilePage, which edits an environment or a
// post-process profile, its factory, and RegisterSettingsProfileEditor, which the editor executable
// calls. It builds on editor.scene: the page shows a scene's settings rows and previews the profile
// on the project's scenes.

export module editor.render;

export import :settings_profile_page;
