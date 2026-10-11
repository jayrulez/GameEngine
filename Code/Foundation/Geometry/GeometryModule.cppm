// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Foundation::Geometry - `foundation.geometry`, the engine's runtime mesh format.
///
/// Distinct from foundation.model (the importer's representation of a loaded file): this
/// is the canonical, GPU-upload-ready mesh the renderer and scene components consume.
/// A converter (tooling, not ported) turns an imported ModelMesh into a StaticMesh /
/// SkinnedMesh. Aggregates the partitions: value types (:types), the index buffer
/// (:index_buffer), the StaticMesh/SkinnedMesh formats (:mesh), and procedural
/// primitives (:primitives).

export module foundation.geometry;

export import :types;
export import :index_buffer;
export import :mesh;
export import :primitives;
export import :mesh_raycast;
