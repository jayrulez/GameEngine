// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell
// Engine::Render tests - the meshes answer rays against what they draw (ISceneSurfaceQuery): the
// nearest hit among them, the hit triangle's nearest corner in world space (scaled and moved),
// the entities a caller turns away, and inactive ones never.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.scene;
import foundation.geometry;
import engine.render;

using namespace foundation::core;
using namespace engine::render;
namespace scene = foundation::scene;
namespace geometry = foundation::geometry;

TEST_CASE("surface query: a ray finds the nearest mesh and the vertex nearest its hit")
{
    scene::Scene world(DefaultAllocator(), u8"world");
    auto* meshes = world.AddSystem<MeshComponentManager>();
    RefPtr<geometry::StaticMesh> cube = geometry::Primitives::Cube(DefaultAllocator(), 2.0f); // faces at +-1

    // A cube at x = 10, doubled: its faces at +-2 about (10, 0, 0).
    const scene::EntityHandle near = world.CreateEntity(u8"near");
    {
        Transform t;
        t.position = Float3{10, 0, 0};
        t.scale = Float3{2, 2, 2};
        world.SetLocalTransform(near, t);
        meshes->Add(near).mesh = cube;
    }
    // A second cube behind it, at x = 20.
    const scene::EntityHandle far = world.CreateEntity(u8"far");
    world.SetLocalPosition(far, Float3{20, 0, 0});
    meshes->Add(far).mesh = cube;

    // Along +x through (0, 0.5, 0.3): the near cube's face at x = 8 first.
    scene::SceneSurfaceHit hit;
    REQUIRE(scene::RaycastSurface(world, Float3{0, 0.5f, 0.3f}, Float3{1, 0, 0}, 100.0f, {}, hit));
    CHECK(hit.entity == near);
    CHECK(hit.distance == doctest::Approx(8.0f));
    CHECK(hit.position.x == doctest::Approx(8.0f));
    CHECK(hit.normal.x == doctest::Approx(-1.0f));
    // The face's corners are (8, +-2, +-2); the one nearest (8, 0.5, 0.3) is (8, 2, 2).
    CHECK(hit.vertex.x == doctest::Approx(8.0f));
    CHECK(hit.vertex.y == doctest::Approx(2.0f));
    CHECK(hit.vertex.z == doctest::Approx(2.0f));

    // Turn the near cube away (a dragged entity never snaps onto itself): the far one.
    REQUIRE(scene::RaycastSurface(world, Float3{0, 0.5f, 0.3f}, Float3{1, 0, 0}, 100.0f,
                                  [near](scene::EntityHandle e) { return e != near; }, hit));
    CHECK(hit.entity == far);
    CHECK(hit.distance == doctest::Approx(19.0f));

    // Out of reach: a miss.
    CHECK_FALSE(scene::RaycastSurface(world, Float3{0, 0.5f, 0.3f}, Float3{1, 0, 0}, 5.0f, {}, hit));

    // An inactive entity draws nothing, so a ray meets nothing of it.
    world.SetActive(near, false);
    REQUIRE(scene::RaycastSurface(world, Float3{0, 0.5f, 0.3f}, Float3{1, 0, 0}, 100.0f, {}, hit));
    CHECK(hit.entity == far);
}
