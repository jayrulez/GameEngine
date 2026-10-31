// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell
// Editor::Core - `editor.core`: the editor-facing half of the editor domain (context, pages,
// commands, selection, actions, thumbnails, per-user settings) over editor.project, which it
// re-exports so an importer of editor.core also sees the project types.
export module editor.core;

export import editor.project;
export import :command;
export import :selection;
export import :search_filter;
export import :editor_settings;
export import :page;
export import :actions;
export import :context;
export import :thumbnail_service;
export import :editor_resources; // the editor's own files, reloaded as they change
export import :editor_icons;     // a type's icon and tint, found by asking the type
