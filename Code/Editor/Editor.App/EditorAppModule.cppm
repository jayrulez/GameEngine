// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::App - the `editor.app` module.
//
// The editor's shell: its window, frame, menus and shortcuts from the actions, the dock and its
// layout, the MCP host and its tools, the project home. On the GUI in parts
// (Documentation/Specs/editor-gui.md, 5.1). Per-subsystem editor plugins register into the
// context from the executable.

export module editor.app;

// The shell's files on the GUI so far (Documentation/Specs/editor-gui.md, 5.1); the rest return
// with their ports.
export import :ui_page;
export import :editor_window;
export import :editor_dock;
export import :native_module;
export import :export_service;
export import :asset_creation;
export import :layout;
export import :session;
export import :mcp_service;
export import :app_actions;
export import :application;
export import :playback_actions;
export import :asset_filter;
export import :property_names;
export import :splash;
export import :font_atlas_cache;
export import :mcp_host;
export import :mcp_operations;
export import :mcp_page_tools;
export import :window_capture;
export import :mcp_window_tools;
export import :mcp_profiler_tools;
