/// Draconic::Render - `draconic.render`, the renderer (scene-agnostic).
///
/// The renderer consumes a per-scene `ExtractedScene` (world-space `RenderData`) and draws
/// the views over it; it knows nothing about the scene/ECS world. The scene-integration
/// layer (components, extraction, the RenderSubsystem) lives in draconic.engine.render and
/// depends on THIS - one-way.
///
/// Partitions: `:data` (the render-data contract + frame arena + radix sort), `:views`
/// (RenderView isolation boundary + pool), `:pipeline` (the Renderer/Pass extension seam +
/// the single per-frame RenderFrame driver), `:mesh_renderer` (the built-in mesh drawer),
/// `:gpu_mesh` (mesh GPU upload cache).

export module draconic.render;

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
export import :taa;
export import :ao;
export import :ssr;
export import :fxaa;
export import :decal_pass;
export import :debug_font;
export import :debug_draw;
export import :debug_pass;

// Per-pass HLSL source partitions (split out of the pass files so shaders are easy to lift to .hlsl
// later; pass-internal, but GCC requires every interface partition be exported by the primary interface).
