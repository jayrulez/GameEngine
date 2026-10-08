// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// SceneRender::lightAt: how much light reaches a point - every enabled light with the renderer's
// falloff and cone, a shadow-casting one stopped by what stands between (the scene's ray query,
// faked here by a wall), plus the ambient. A game's light meter and a guard's eye read it.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.scene;
import foundation.render;
import engine.render;

using namespace foundation::core;
using namespace engine::render;
namespace scene = foundation::scene;

namespace
{
    bool Near(f32 a, f32 b) { return Abs(a - b) < 1e-4f; }

    // A wall across the plane x = `wallX`, answered as the scene's solid-surface ray query (the
    // seam physics fills in a running game); remembers the group mask it was asked with.
    class Wall final : public scene::SceneSystem, public scene::ISceneRayQuery
    {
    public:
        [[nodiscard]] scene::ISceneRayQuery* AsRayQuery() noexcept override { return this; }
        bool CastRay(Float3 origin, Float3 direction, f32 maxDistance, u32 groupMask,
                     scene::SceneRayHit& out) override
        {
            lastMask = groupMask;
            if (Abs(direction.x) < 1.0e-6f)
            {
                return false;
            }
            const f32 t = (wallX - origin.x) / direction.x;
            if (t < 0.0f || t > maxDistance)
            {
                return false;
            }
            out.distance = t;
            out.position = origin + direction * t;
            return true;
        }
        f32 wallX = 1.0f;
        u32 lastMask = 0;
    };

    scene::EntityHandle Place(scene::Scene& s, Float3 at, Quaternion rotation = Quaternion::Identity)
    {
        const scene::EntityHandle e = s.CreateEntity();
        Transform t;
        t.position = at;
        t.rotation = rotation;
        s.SetLocalTransform(e, t);
        return e;
    }
}

TEST_CASE("lightAt sums each enabled light by the renderer's range falloff, plus the ambient")
{
    scene::Scene s(DefaultAllocator(), u8"lightAt");
    auto* lights = s.AddSystem<LightComponentManager>();
    auto* env = s.AddSystem<EnvironmentSystem>();
    env->Environment().ambientColor = Color{1.0f, 1.0f, 1.0f, 1.0f};
    env->Environment().ambientIntensity = 0.1f;
    LightComponent& lamp = lights->Add(Place(s, Float3{0.0f, 2.0f, 0.0f}));
    lamp.type = LightType::Point;
    lamp.color = Color{1.0f, 1.0f, 1.0f, 1.0f};
    lamp.intensity = 4.0f;
    lamp.range = 10.0f;
    s.UpdateTransforms();
    const SceneRender render{&s};

    // 2 m below a lamp of range 10: d = 0.2, window (1 - d^4)^2 = 0.99680256, over 4 m^2.
    const f32 expected = 4.0f * 0.99680256f / (4.0f + 1e-4f) + 0.1f;
    const Float3 lit = render.lightAt(Float3{0.0f, 0.0f, 0.0f});
    CHECK(Near(lit.x, expected));
    CHECK(Near(lit.y, expected));
    CHECK(Near(lit.z, expected));
    // Past its range: the ambient alone.
    CHECK(Near(render.lightAt(Float3{0.0f, 2.0f, 12.0f}).x, 0.1f));
    // Switched off: the ambient alone.
    lamp.enabled = false;
    CHECK(Near(render.lightAt(Float3{0.0f, 0.0f, 0.0f}).x, 0.1f));
}

TEST_CASE("lightAt follows a spot's cone: full inside the inner angle, nothing past the outer")
{
    scene::Scene s(DefaultAllocator(), u8"lightAt.spot");
    auto* lights = s.AddSystem<LightComponentManager>();
    // Pointing straight down: forward is -Z, turned a quarter about X.
    LightComponent& spot = lights->Add(
        Place(s, Float3{0.0f, 0.0f, 0.0f}, Quaternion::FromAxisAngle(Float3{1, 0, 0}, -1.5707963f)));
    spot.type = LightType::Spot;
    spot.intensity = 1.0f;
    spot.range = 0.0f; // no range falloff: the cone alone
    spot.innerAngle = 0.3f;
    spot.outerAngle = 0.5f;
    s.UpdateTransforms();
    const SceneRender render{&s};

    CHECK(Near(render.lightAt(Float3{0.0f, -2.0f, 0.0f}).x, 1.0f));                    // on the axis
    CHECK(Near(render.lightAt(Float3{2.0f * Tan(0.7f), -2.0f, 0.0f}).x, 0.0f));        // outside
    const f32 half = render.lightAt(Float3{2.0f * Tan(0.4f), -2.0f, 0.0f}).x;          // in the edge
    CHECK(half > 0.1f);
    CHECK(half < 0.9f);
}

TEST_CASE("lightAt: a wall stops a shadow-casting light, by its shadow strength; a light without shadows shines through")
{
    scene::Scene s(DefaultAllocator(), u8"lightAt.wall");
    auto* lights = s.AddSystem<LightComponentManager>();
    auto* wall = s.AddSystem<Wall>(); // across x = 1
    LightComponent& lamp = lights->Add(Place(s, Float3{2.0f, 0.0f, 0.0f}));
    lamp.type = LightType::Point;
    lamp.range = 0.0f; // no range: the shader applies no falloff at all, so 4 arrives
    lamp.intensity = 4.0f;
    s.UpdateTransforms();
    const SceneRender render{&s};
    const Float3 here{0.0f, 0.0f, 0.0f};

    // No shadows: the wall does not matter, as it does not on screen.
    CHECK(Near(render.lightAt(here).x, 4.0f));
    lamp.castsShadows = true;
    CHECK(Near(render.lightAt(here).x, 0.0f));
    lamp.shadowStrength = 0.25f; // a light shadow: three quarters still arrive
    CHECK(Near(render.lightAt(here).x, 3.0f));
    // On the lamp's side of the wall nothing stands between.
    lamp.shadowStrength = 1.0f;
    CHECK(render.lightAt(Float3{1.5f, 0.0f, 0.0f}).x > 1.0f);
    // The caller's collision groups reach the ray.
    (void)render.lightAt(here, 0x5u);
    CHECK(wall->lastMask == 0x5u);
}

TEST_CASE("lightAt: a directional light reaches everywhere unless something is overhead toward it")
{
    scene::Scene s(DefaultAllocator(), u8"lightAt.sun");
    auto* lights = s.AddSystem<LightComponentManager>();
    auto* wall = s.AddSystem<Wall>();
    // Shining along +X (forward -Z, turned a quarter about Y), so the way to it is -X.
    LightComponent& sun = lights->Add(
        Place(s, Float3{0.0f, 0.0f, 0.0f}, Quaternion::FromAxisAngle(Float3{0, 1, 0}, -1.5707963f)));
    sun.type = LightType::Directional;
    sun.intensity = 0.5f;
    sun.castsShadows = true;
    s.UpdateTransforms();
    const SceneRender render{&s};

    wall->wallX = 5.0f; // behind, along the light: no matter
    CHECK(Near(render.lightAt(Float3{0.0f, 0.0f, 0.0f}).x, 0.5f));
    wall->wallX = -5.0f; // between the point and the light
    CHECK(Near(render.lightAt(Float3{0.0f, 0.0f, 0.0f}).x, 0.0f));
}
