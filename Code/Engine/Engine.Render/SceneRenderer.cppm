// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Engine::Render - the `:scene_renderer` partition.
///
/// `ISceneRenderer` (with CameraOverride and TargetState) lives in the light
/// `foundation.render.api` module so tools can drive scene rendering through the interface
/// without linking the renderer (the editor shell's layering rule). This partition re-exports
/// it for subsystem code and importers; RenderSubsystem is the implementation.

export module engine.render:scene_renderer;

export import foundation.render.api;
