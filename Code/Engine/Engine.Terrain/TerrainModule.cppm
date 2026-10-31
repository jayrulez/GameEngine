// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// engine.terrain - the terrain module's public face: the ECS component and manager (which provides
// the chunk render data), the GPU height-texture cache, the render-data payload, the chunked
// geo-mipmap TerrainRenderer, and the application-level TerrainSubsystem that wires them into
// RenderSubsystem through its generic register-renderer and register-provider hooks, so
// Engine.Render has no terrain code. Physics is a separate ShapeKind::Heightfield collider over the
// same shared heightfield.

export module engine.terrain;

export import :heighttexture; // the GPU height-texture cache
export import :splattexture;  // the GPU splat-texture cache (RGBA8, paint re-upload)
export import :renderdata;    // TerrainRenderData (the per-terrain draw-list payload)
export import :holedmesh;     // TerrainHoledMeshCache (holed chunks' own index buffers)
export import :components;    // TerrainComponent and manager (IRenderDataProvider)
export import :renderer;      // TerrainRenderer (dedicated Opaque Renderer)
export import :subsystem;     // TerrainSubsystem (registers renderer and provider)
