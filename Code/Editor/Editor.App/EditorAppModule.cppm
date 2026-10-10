// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::App - the `editor.app` module.
//
// The editor UI shell on foundation.ui: the EditorShell chrome
// (MenuBar / DockManager / StatusBar + the five standard panels), per-user dock-layout
// persistence, and EditorApplication (the runtime IApplication assembling UIHost +
// RuntimeDockableWindowHost + EditorContext/EditorProject). Per-subsystem editor plugins
// register into the context from the executable.

export module editor.app;

export import :layout;
export import :log_view;
export import :ui_page;
export import :page_toolbar;
export import :playback_actions;
export import :action_menus;
export import :action_shortcuts;
export import :tool_panel;
export import :tool_panel_widgets;
export import :assets_view;
export import :asset_picker_dialog;
export import :asset_create_dialog;
export import :confirm_dialog;
export import :path_picker_dialog;
    export import :group_picker_dialog;
export import :import_dialog;
export import :settings_dialog;
export import :preferences_dialog;
export import :command_palette;
export import :shortcut_capture;
export import :editor_icons;
    export import :asset_picker_slot;
    export import :container_list_editor;
    export import :resource_ref_editor;
    export import :compact_asset_slot;
    export import :list_header;
    export import :view_mode_toggles;
export import :asset_drag_data;
export import :project_manager_view;
export import :shell;
export import :font_atlas_cache;
export import :mcp_host;
export import :mcp_operations;
export import :mcp_page_tools;
export import :window_capture;
export import :mcp_window_tools;
export import :mcp_action_tools;
export import :application;
