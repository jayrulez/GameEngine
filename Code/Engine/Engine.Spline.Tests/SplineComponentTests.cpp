// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// SplineComponent: serialization round-trips the authored point set (positions, handles,
// modes, closed flag) and rebuilds the derived arc-length cache on load.

#include <doctest/doctest.h>

#include "Core/Prelude.h"
#include <initializer_list>

import foundation.core;
import foundation.scene;
import foundation.spline;
import foundation.script.facades;
import engine.spline;

using namespace foundation::core;
using namespace foundation::spline;
using engine::spline::SplineComponent;

TEST_CASE("spline component: serialization round-trips points and rebuilds caches")
{
    SplineComponent authored;
    authored.curve.points.PushBack(SplinePoint{Float3{0, 0, 0}});
    authored.curve.points.PushBack(
        SplinePoint{Float3{5, 1, 0}, Float3{-1, 0, 0}, Float3{1, 0, 0},
                    SplineHandleMode::Broken});
    authored.curve.points.PushBack(SplinePoint{Float3{10, 0, 4}});
    authored.curve.closed = true;
    authored.curve.UpdateAutoHandles();
    authored.curve.RebuildArcLength();

    MemoryStream buffer;
    {
        BinarySerializer writer(buffer, SerializeMode::Write);
        Serialize(writer, authored);
        REQUIRE(writer.IsOk());
    }
    (void)buffer.Seek(0, SeekOrigin::Begin);
    SplineComponent loaded;
    {
        BinarySerializer reader(buffer, SerializeMode::Read);
        Serialize(reader, loaded);
        REQUIRE(reader.IsOk());
    }

    REQUIRE(loaded.curve.points.Size() == 3u);
    CHECK(loaded.curve.closed);
    CHECK(Length(loaded.curve.points[1].position - Float3{5, 1, 0}) < 0.0001f);
    CHECK(Length(loaded.curve.points[1].inHandle - Float3{-1, 0, 0}) < 0.0001f);
    CHECK(loaded.curve.points[1].mode == SplineHandleMode::Broken);
    // Derived caches rebuilt on read: the loaded curve evaluates identically.
    CHECK(loaded.curve.Length() == doctest::Approx(authored.curve.Length()).epsilon(0.001));
    CHECK(Length(loaded.curve.Evaluate(1.5f) - authored.curve.Evaluate(1.5f)) < 0.0001f);
}

TEST_CASE("spline component: added bare it is seeded at its first Initialize; loaded points are kept")
{
    foundation::scene::Scene world(DefaultAllocator(), u8"t");
    auto* splines = world.AddSystem<engine::spline::SplineComponentManager>();

    // Add Component in the editor, or a script: no points until the Initialize phase runs.
    const foundation::scene::EntityHandle bare = world.CreateEntity(u8"bare");
    engine::spline::SplineComponent& added = splines->Add(bare);
    CHECK(added.PointCount() == 0u);
    // A component that already has its points (a load, a spawn) is not touched.
    const foundation::scene::EntityHandle loaded = world.CreateEntity(u8"loaded");
    engine::spline::SplineComponent& kept = splines->Add(loaded);
    for (const f32 x : {0.0f, 1.0f, 2.0f})
    {
        SplinePoint point;
        point.position = Float3{x, 0.0f, 0.0f};
        kept.curve.points.PushBack(point);
    }
    world.InitializePendingComponents();
    CHECK(splines->Get(bare)->PointCount() == 2u); // the seed: a segment along local X
    CHECK(splines->Get(bare)->curve.points[0].position.x == -1.0f);
    CHECK(splines->Get(bare)->curve.points[1].position.x == 1.0f);
    CHECK(splines->Get(bare)->curve.Length() > 1.9f); // caches rebuilt with the seed
    CHECK(splines->Get(loaded)->PointCount() == 3u);
    CHECK(splines->Get(loaded)->curve.points[0].position.x == 0.0f);

    // The inspector's rows: the loop flag goes through the curve and rebuilds its arc length.
    engine::spline::SplineComponent& three = *splines->Get(loaded);
    three.curve.UpdateAutoHandles();
    three.curve.RebuildArcLength();
    const f32 open = three.curve.Length();
    CHECK_FALSE(three.IsClosed());
    three.SetClosed(true);
    CHECK(three.IsClosed());
    CHECK(three.curve.Length() > open); // the closing segment is in the table now
    three.SetClosed(false);
    CHECK(three.curve.Length() == doctest::Approx(open));
}

TEST_CASE("spline facade: world-space queries through SceneSplines")
{
    foundation::scene::Scene sceneObj(DefaultAllocator(), u8"splines");
    engine::spline::AddSplineSceneManagers(sceneObj);
    auto* manager = sceneObj.GetSystem<engine::spline::SplineComponentManager>();
    REQUIRE(manager != nullptr);

    const foundation::scene::EntityHandle entity = sceneObj.CreateEntity(u8"path");
    engine::spline::SplineComponent& component = manager->Add(entity);
    component.curve.points.PushBack(SplinePoint{Float3{0, 0, 0}});
    component.curve.points.PushBack(SplinePoint{Float3{10, 0, 0}});
    component.curve.UpdateAutoHandles();
    component.curve.RebuildArcLength();

    // Place the entity: the facade must answer in WORLD space.
    Transform t;
    t.position = Float3{0, 5, 0};
    sceneObj.SetLocalTransform(entity, t);
    sceneObj.UpdateTransforms();

    foundation::script::Entity scriptEntity{&sceneObj, entity.index, entity.generation};
    engine::spline::SceneSplines splines{&sceneObj};

    CHECK(splines.pointCount(scriptEntity) == 2);
    CHECK(!splines.isClosed(scriptEntity));
    CHECK(splines.length(scriptEntity) == doctest::Approx(10.0f).epsilon(0.001));

    const engine::spline::SplineHit mid = splines.sampleAtDistance(scriptEntity, 5.0f);
    CHECK(mid.valid);
    CHECK(Length(mid.position - Float3{5, 5, 0}) < 0.05f);
    CHECK(Length(mid.tangent - Float3{1, 0, 0}) < 0.01f);

    const engine::spline::SplineHit nearest = splines.closestPoint(scriptEntity, 3.0f, 9.0f, 0.0f);
    // The Float3 form (Sedulous's ClosestPoint) answers the same.
    const engine::spline::SplineHit byFloat3 =
        splines.closestPoint(scriptEntity, Float3{3.0f, 9.0f, 0.0f});
    CHECK(byFloat3.valid == nearest.valid);
    CHECK(byFloat3.t == doctest::Approx(nearest.t));
    CHECK(nearest.valid);
    CHECK(Length(nearest.position - Float3{3, 5, 0}) < 0.05f);
    // How far down the line the closest point is, and the distance a sample was asked at.
    CHECK(nearest.distance == doctest::Approx(3.0f).epsilon(0.02));
    CHECK(mid.distance == doctest::Approx(5.0f).epsilon(0.001));

    // No spline on the entity -> the invalid hit, zeroed.
    const foundation::scene::EntityHandle bare = sceneObj.CreateEntity(u8"bare");
    foundation::script::Entity bareEntity{&sceneObj, bare.index, bare.generation};
    CHECK(!splines.sampleAt(bareEntity, 0.5f).valid);
    CHECK(splines.length(bareEntity) == 0.0f);
}

TEST_CASE("path follow: the follower advances, aligns, and stops at an open end")
{
    foundation::scene::Scene sceneObj(DefaultAllocator(), u8"follow");
    engine::spline::AddSplineSceneManagers(sceneObj);
    auto* splines = sceneObj.GetSystem<engine::spline::SplineComponentManager>();
    auto* follows = sceneObj.GetSystem<engine::spline::PathFollowComponentManager>();
    REQUIRE(splines != nullptr);
    REQUIRE(follows != nullptr);
    sceneObj.SetSimulationEnabled(true);

    const foundation::scene::EntityHandle path = sceneObj.CreateEntity(u8"path");
    engine::spline::SplineComponent& spline = splines->Add(path);
    spline.curve.points.PushBack(SplinePoint{Float3{0, 0, 0}});
    spline.curve.points.PushBack(SplinePoint{Float3{10, 0, 0}});
    spline.curve.UpdateAutoHandles();
    spline.curve.RebuildArcLength();

    const foundation::scene::EntityHandle mover = sceneObj.CreateEntity(u8"mover");
    engine::spline::PathFollowComponent& follow = follows->Add(mover);
    follow.spline = sceneObj.GetEntityId(path);
    follow.speed = 2.0f;
    follow.loop = false;
    sceneObj.UpdateTransforms();

    sceneObj.Update(1.0f); // 2 units along +X
    const Float3 afterOne = sceneObj.GetWorldPosition(mover);
    CHECK(afterOne.x == doctest::Approx(2.0f).epsilon(0.05));
    CHECK(afterOne.y == doctest::Approx(0.0f).epsilon(0.01));
    // Aligned: -Z of the follower's rotation points along +X (the tangent).
    const Transform t = sceneObj.GetLocalTransform(mover);
    const Float3 forward = RotateVector(t.rotation, Float3{0, 0, -1});
    CHECK(Length(forward - Float3{1, 0, 0}) < 0.01f);

    // Run past the end: clamps at the far point and stops playing.
    for (int i = 0; i < 10; ++i)
    {
        sceneObj.Update(1.0f);
    }
    const Float3 end = sceneObj.GetWorldPosition(mover);
    CHECK(end.x == doctest::Approx(10.0f).epsilon(0.01));
    CHECK(!follows->Get(mover)->playing);
}

TEST_CASE("path follow: a looping follower wraps instead of stopping")
{
    foundation::scene::Scene sceneObj(DefaultAllocator(), u8"follow_loop");
    engine::spline::AddSplineSceneManagers(sceneObj);
    auto* splines = sceneObj.GetSystem<engine::spline::SplineComponentManager>();
    auto* follows = sceneObj.GetSystem<engine::spline::PathFollowComponentManager>();
    sceneObj.SetSimulationEnabled(true);

    const foundation::scene::EntityHandle path = sceneObj.CreateEntity(u8"path");
    engine::spline::SplineComponent& spline = splines->Add(path);
    spline.curve.points.PushBack(SplinePoint{Float3{0, 0, 0}});
    spline.curve.points.PushBack(SplinePoint{Float3{10, 0, 0}});
    spline.curve.UpdateAutoHandles();
    spline.curve.RebuildArcLength();

    const foundation::scene::EntityHandle mover = sceneObj.CreateEntity(u8"mover");
    engine::spline::PathFollowComponent& follow = follows->Add(mover);
    follow.spline = sceneObj.GetEntityId(path);
    follow.speed = 4.0f;
    follow.loop = true;
    sceneObj.UpdateTransforms();

    sceneObj.Update(3.0f); // 12 units -> wraps to 2
    CHECK(sceneObj.GetWorldPosition(mover).x == doctest::Approx(2.0f).epsilon(0.05));
    CHECK(follows->Get(mover)->playing);
}
