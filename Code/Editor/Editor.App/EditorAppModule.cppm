// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::App - the `editor.app` module.
//
// The editor's shell: its window, frame, menus and shortcuts from the actions, the dock and its
// layout, the MCP host and its tools, the project home. Per-subsystem editor plugins register into
// the context from the executable.

export module editor.app;

// The shell's partitions.
export import :ui_page;
export import :page_action_bar;
export import :property_sections;
export import :property_extras;
export import :tool_panel;
export import :tool_panel_widgets;
export import :status_bar;
export import :editor_window;
export import :editor_dock;
export import :pages;
export import :action_menus;
export import :action_shortcuts;
export import :command_palette;
export import :shortcut_capture;
export import :preferences_dialog;
export import :project_picture;
export import :project_home;
export import :project_manager_view;
export import :log_view;
export import :profiler_panel;
export import :asset_drag_data;
export import :asset_picker_slot;
export import :resource_ref_editor;
export import :asset_page;
export import :image_inspect_view;
export import :asset_object_page;
export import :preview_pane;
export import :picker;
export import :path_picker_dialog;
export import :asset_picker_window;
export import :group_picker_dialog;
export import :asset_create_dialog;
export import :settings_dialog;
export import :templates_dialog;
export import :export_dialog;
export import :asset_operations;
export import :asset_import;
export import :import_dialog;
export import :assets_view;
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
export import :mcp_layout_tools;
export import :mcp_action_tools;
export import :window_capture;
export import :mcp_window_tools;
export import :mcp_input_tools;
export import :code_languages;
export import :problems_list;
export import :code_asset_page;
export import :mcp_profiler_tools;
