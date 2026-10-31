// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Foundation::Render - `foundation.render`, the renderer (scene-agnostic).
///
/// The renderer consumes a per-scene `ExtractedScene` (world-space `RenderData`) and draws
/// the views over it; it knows nothing about the scene or ECS world. The scene-integration
/// layer (components, extraction, the RenderSubsystem) lives in engine.render and
/// depends on this, never the reverse.
///
/// The core partitions are `:data` (the render-data contract, frame arena and radix sort),
/// `:views` (the RenderView isolation boundary and pool), `:pipeline` (the Renderer extension
/// point and the single per-frame RenderFrame driver), `:mesh_renderer` (the built-in mesh
/// drawer) and `:gpu_mesh` (mesh GPU upload cache); the rest are one per pass or system.

export module foundation.render;

export import :data;
export import :views;
export import :extract_ctx;
export import :resources;
export import :pipeline;
export import :gpu_mesh;
export import :mesh_renderer;
export import :sprite_renderer;
export import :cluster_system;
export import :tonemap;
export import :shadows;
export import :ibl;
export import :probes;
export import :sky;
export import :bloom;
export import :bind_group_cache;
export import :taa;
export import :ao;
export import :msaa_resolve;
export import :picking;
export import :ssr;
export import :ssgi;
export import :fxaa;
export import :debug_blit;
export import :exposure;
export import :decal_pass;
export import :debug_font;
export import :debug_draw;
export import :debug_pass;
