// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// foundation.vegetation: the terrain vegetation model: the layer parameters and the pure,
// deterministic per-chunk scatter with its distance-fade math. No RHI, no scene.

export module foundation.vegetation;

export import :layer;   // VegetationPlacement, ScatterLayer, LayerScatterHash
export import :scatter; // ScatterChunk, ChunkSeed, DensityAtDistance, FadePrefix, ChunksTouchedBy
