// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Scene - the `editor.scene` module.
//
// The scene subsystem's editor plugin: SceneEditorPage (per-page live Scene +
// ViewportView through the real renderer + EditorCamera), its page factory, and the
// RegisterSceneEditor entry point the editor EXECUTABLE calls - the editor core/app never link
// this module.

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
export import :pie_page_interface;
export import :pie_tools;
export import :gizmo;
export import :tools;
export import :component_gizmos;
export import :asset_thumbnails;
export import :hierarchy;
export import :inspector;
export import :entity_picker_dialog;
export import :component_picker_dialog;
export import :page;
export import :game_page;
export import :game_resolution;
export import :material_page;
export import :settings_profile_page;
export import :mesh_page;
export import :particle_effect_page;
export import :animation_graph_page;
export import :animation_clip_page;
export import :skeleton_page;
