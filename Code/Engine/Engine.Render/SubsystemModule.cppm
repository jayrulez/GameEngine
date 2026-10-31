// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Engine::Render - `engine.render`, the scene side of rendering.
///
/// Render components and their managers, render profiles, and the extraction that pushes a
/// render::ExtractedScene to the renderer. This is where scene and renderer meet: it depends on
/// both foundation.scene and foundation.render; the renderer depends on neither. The
/// RenderSubsystem (:subsystem) contributes the managers through the composition and each frame
/// extracts and draws.

export module engine.render;

export import :components;
export import :profiles;
export import :scene_renderer;
export import :extract;
export import :subsystem;
