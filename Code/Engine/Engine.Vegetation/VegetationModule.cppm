// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// engine.vegetation - the terrain vegetation module's public face: the per-layer scene component
// and its manager (the render-data provider that scatters per chunk and emits instanced sets), and
// the application-level VegetationSubsystem that registers the manager as a render provider per
// scene.

export module engine.vegetation;

export import :components; // TerrainVegetationComponent and manager (IRenderDataProvider)
export import :subsystem;  // VegetationSubsystem (registers the provider per scene)
