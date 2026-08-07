// draconic.editor.scene: the UI-free core of the scene-editor camera preview (task #118) - the
// CameraOverride built from a CameraComponent + world matrix, and the visibility/pin decision.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"

import draconic.foundation;
import draconic.scene;
import draconic.render.api;
import draconic.engine.render;
import draconic.editor.scene;

using namespace draconic::foundation;
namespace scene = draconic::scene;
namespace render = draconic::render;
namespace editor = draconic::editor;

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
    render::CameraComponent cam;
    cam.fovYRadians = 1.0f;
    cam.aspect = 4.0f / 3.0f;
    cam.nearZ = 0.5f;
    cam.farZ = 250.0f;
    cam.clearColor = Color{0.1f, 0.2f, 0.3f, 1.0f};

    const Float4x4 world = Float4x4::Translation(Float3{10.0f, 5.0f, 3.0f});
    const render::CameraOverride ov = editor::BuildCameraPreviewOverride(cam, world);

    // view = inverse(world); projection = the RH perspective from the lens fields (matches the
    // runtime primary-camera extraction); position = the world origin; farZ + clearColor carried.
    CHECK(MatNear(ov.camera.view, Inverse(world)));
    CHECK(MatNear(ov.camera.projection,
                  Float4x4::PerspectiveFovRH(1.0f, 4.0f / 3.0f, 0.5f, 250.0f)));
    CHECK(ov.camera.position.x == doctest::Approx(10.0f));
    CHECK(ov.camera.position.y == doctest::Approx(5.0f));
    CHECK(ov.camera.position.z == doctest::Approx(3.0f));
    CHECK(ov.camera.farZ == doctest::Approx(250.0f));
    CHECK(ov.clearColor.r == doctest::Approx(0.1f));
    CHECK(ov.clearColor.b == doctest::Approx(0.3f));
    CHECK(ov.clearColor.a == doctest::Approx(1.0f));
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
