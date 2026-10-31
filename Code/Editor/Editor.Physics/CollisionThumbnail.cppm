// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor Physics - :collision_thumbnail partition.
//
// GPU thumbnail generator for collision shapes: the cooked product's outline triangles (the
// same soup the collider gizmos draw) rebuilt as a renderable mesh with flat facets, framed by
// its bounds. It reads only data: the cooked outline as it is, never a physics world or a Jolt
// shape. RegisterCollisionShapeEditor registers it, and the stage does all the GPU work.

module;
#include "Core/Prelude.h"

export module editor.physics:collision_thumbnail;

import foundation.core;
import foundation.geometry;         // StaticMesh (the outline as a lit mesh)
import foundation.physics.resource; // CollisionShape (the cooked outline)
import editor.core;

using namespace foundation::core;

export namespace editor
{
    /// The cooked outline (a triangle soup, 3 vertices each) as a lit StaticMesh: unwelded,
    /// so the generated normals give the flat facets a hull should read as; one submesh; a
    /// stray vertex past the last whole triangle is dropped. False (and an empty mesh) when
    /// there is no whole triangle.
    [[nodiscard]] bool BuildCollisionOutlineMesh(const foundation::physics::CollisionShape& shape,
                                                 foundation::geometry::StaticMesh& mesh);

    /// Creates the generator for a host that keeps its own (a test). It is the same generator that
    /// RegisterCollisionThumbnailGenerator hands the service.
    [[nodiscard]] UniquePtr<ISceneThumbnailGenerator> CreateCollisionThumbnailGenerator();

    /// Register the collision-shape thumbnail generator (the domain's composition entry
    /// point calls this; the count feeds the composition root's tripwire).
    void RegisterCollisionThumbnailGenerator(ThumbnailService& service);
}
