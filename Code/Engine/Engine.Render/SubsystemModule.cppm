/// Engine::Render - `engine.render`, the scene side of rendering.
///
/// Render components (mesh / camera) + their managers, and the extraction that pushes a
/// engine::render::ExtractedView to the renderer. This is where scene and renderer meet - it
/// depends on both foundation.scene and foundation.render; the renderer depends on neither. A
/// later partition adds the RenderSubsystem (contributes the managers via the composition, and
/// each frame extracts + draws).

export module engine.render;

export import :components;
export import :scene_renderer;
export import :extract;
export import :subsystem;
