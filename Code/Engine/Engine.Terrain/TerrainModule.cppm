// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// engine.terrain - the terrain module's public face: the ECS component + manager (which provides the
// chunk render-data), the GPU height-texture cache, the render-data payload, the chunked geo-mipmap
// TerrainRenderer, and the application-level TerrainSubsystem that wires them into RenderSubsystem via its
// generic register-renderer / register-provider seam. Engine.Render stays terrain-free. Physics is a
// SEPARATE ShapeKind::Heightfield collider over the same shared heightfield.

export module engine.terrain;

export import :heighttexture; // the GPU height-texture cache
export import :splattexture;  // the GPU splat-texture cache (RGBA8, paint re-upload)
export import :renderdata;    // TerrainRenderData (the per-terrain draw-list payload)
export import :holedmesh;     // TerrainHoledMeshCache (holed chunks' own index buffers)
export import :components;    // TerrainComponent + manager (IRenderDataProvider)
export import :renderer;      // TerrainRenderer (dedicated Opaque Renderer)
export import :subsystem;     // TerrainSubsystem (registers renderer + provider)
