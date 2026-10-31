// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// editor.scene: the UI-free core of the scene editor's camera preview: the CameraOverride built
// from a CameraComponent and a world matrix, and the decision to show or pin the preview.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.scene;
import foundation.render.api;
import engine.render;
import editor.scene;

using namespace foundation::core;
namespace scene = foundation::scene;
namespace render = foundation::render;

namespace
{
    bool MatNear(const Float4x4& a, const Float4x4& b)
    {
        for (usize r = 0; r < 4; ++r)
        {
            for (usize c = 0; c < 4; ++c)
            {
                if (Abs(a(r, c) - b(r, c)) > 1e-4f)
                {
                    return false;
                }
            }
        }
        return true;
    }
}

TEST_CASE("camera-preview: BuildCameraPreviewOverride maps a component + world to a CameraOverride")
{
    engine::render::CameraComponent cam;
    cam.fovYRadians = 1.0f;
    cam.aspect = 4.0f / 3.0f;
    cam.nearZ = 0.5f;
    cam.farZ = 250.0f;
    cam.clearColor = Color{0.1f, 0.2f, 0.3f, 1.0f};

    const Float4x4 world = Float4x4::Translation(Float3{10.0f, 5.0f, 3.0f});
    const render::CameraOverride ov = editor::BuildCameraPreviewOverride(cam, world);

    // The view is the inverse of the world matrix and the projection is the right-handed
    // perspective from the lens fields, as the runtime primary-camera extraction builds them. The
    // position is the world origin, farZ carries over, and the clear colour (authored in sRGB) is
    // decoded to linear as the runtime does.
    CHECK(MatNear(ov.camera.view, Inverse(world)));
    CHECK(MatNear(ov.camera.projection,
                  Float4x4::PerspectiveFovRH(1.0f, 4.0f / 3.0f, 0.5f, 250.0f)));
    CHECK(ov.camera.position.x == doctest::Approx(10.0f));
    CHECK(ov.camera.position.y == doctest::Approx(5.0f));
    CHECK(ov.camera.position.z == doctest::Approx(3.0f));
    CHECK(ov.camera.farZ == doctest::Approx(250.0f));
    CHECK(ov.clearColor.r == doctest::Approx(SrgbToLinear(0.1f)));
    CHECK(ov.clearColor.b == doctest::Approx(SrgbToLinear(0.3f)));
    CHECK(ov.clearColor.a == doctest::Approx(1.0f));
}

TEST_CASE("camera-preview: an orthographic camera previews orthographic, like the game draws it")
{
    engine::render::CameraComponent cam;
    cam.aspect = 2.0f;
    cam.projection = engine::render::CameraProjection::Orthographic;
    cam.orthoHeight = 12.0f;
    const render::CameraOverride ov = editor::BuildCameraPreviewOverride(cam, Float4x4::Identity());
    CHECK(MatNear(ov.camera.projection, engine::render::MakeCameraProjection(cam, 2.0f)));
    CHECK(MatNear(ov.camera.projection,
                  Float4x4::OrthographicRH(24.0f, 12.0f, cam.nearZ, cam.farZ)));
}

TEST_CASE("camera-preview: ResolveCameraPreview visibility + pin logic")
{
    const scene::EntityHandle camEntity{1, 1};
    const scene::EntityHandle meshEntity{2, 1};
    const scene::EntityHandle none = scene::EntityHandle::Invalid();

    SUBCASE("camera selected, no pin -> visible on the selection")
    {
        const auto r = editor::ResolveCameraPreview(camEntity, true, none, false);
        CHECK(r.visible);
        CHECK(r.target == camEntity);
        CHECK_FALSE(r.unpin);
    }
    SUBCASE("non-camera selected, no pin -> hidden")
    {
        const auto r = editor::ResolveCameraPreview(meshEntity, false, none, false);
        CHECK_FALSE(r.visible);
        CHECK_FALSE(r.target.IsAssigned());
        CHECK_FALSE(r.unpin);
    }
    SUBCASE("nothing selected, no pin -> hidden")
    {
        const auto r = editor::ResolveCameraPreview(none, false, none, false);
        CHECK_FALSE(r.visible);
    }
    SUBCASE("pinned (valid) beats a different selection")
    {
        const auto r = editor::ResolveCameraPreview(meshEntity, false, camEntity, true);
        CHECK(r.visible);
        CHECK(r.target == camEntity);
        CHECK_FALSE(r.unpin);
    }
    SUBCASE("pinned (valid) survives deselection")
    {
        const auto r = editor::ResolveCameraPreview(none, false, camEntity, true);
        CHECK(r.visible);
        CHECK(r.target == camEntity);
    }
    SUBCASE("pinned entity went stale + non-camera selection -> hide + auto-unpin")
    {
        const auto r = editor::ResolveCameraPreview(meshEntity, false, camEntity, false);
        CHECK_FALSE(r.visible);
        CHECK(r.unpin);
    }
    SUBCASE("pinned stale + a camera IS selected -> show the selection + clear the stale pin")
    {
        const auto r = editor::ResolveCameraPreview(camEntity, true, meshEntity, false);
        CHECK(r.visible);
        CHECK(r.target == camEntity);
        CHECK(r.unpin);
    }
}
