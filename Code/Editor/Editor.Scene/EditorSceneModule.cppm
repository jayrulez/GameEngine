// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Scene - the `editor.scene` module.
//
// The scene subsystem's editor plugin: SceneEditorPage (a live Scene per page, a ViewportView
// through the real renderer, and an EditorCamera), its page factory, and the RegisterSceneEditor
// entry point the editor executable calls. The editor core and app never link this module.

export module editor.scene;

export import editor.camera;
export import :view_settings;
export import :zoom_readout;
export import :view_gizmo;
export import :scene_grid;
export import :camera_preview;
export import :edit;
export import :settings_profiles;
export import :scene_page_interface;
export import :actions;
export import :entity_json;
export import :mcp_tools;
export import :viewport_capture;
export import :scene_loading;
export import :gizmo;
export import :tools;
export import :component_gizmos;
export import :asset_thumbnails;
export import :services;
export import :scene_overlays;
export import :scene_view_state;
export import :scene_simulation;
export import :prefab_flows;
export import :scene_tree;
export import :hierarchy;
export import :entity_picker_window;
export import :entity_ref_editor;
export import :bone_name_editor;
export import :collision_matrix_editor;
export import :inspector_extras;
export import :script_behaviors_source;
export import :scene_settings_source;
export import :settings_extra_sources;
export import :builtin_inspector_extras;
export import :inspector;
export import :scene_bar;
export import :scene_hud;
export import :scene_camera_preview;
export import :scene_tools;
export import :scene_viewport;
export import :page;
export import :registration;
