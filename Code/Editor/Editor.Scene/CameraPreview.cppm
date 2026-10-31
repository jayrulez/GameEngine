// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Scene - :camera_preview partition.
//
// The testable core of the scene editor's camera preview, free of UI: the CameraOverride a
// selected or pinned CameraComponent renders through, and the visibility and pin decision. The
// preview view class and its ScenePage wiring live in the implementation units; these pure
// functions carry the logic so it can be unit-tested without a renderer or a UI tree.

module;
#include "Core/Prelude.h"

export module editor.scene:camera_preview;

import foundation.core;
import foundation.scene;
import foundation.render.api;    // ViewCamera / CameraOverride
import engine.render; // CameraComponent

using namespace foundation::core;

export namespace editor
{
    namespace scene = foundation::scene;
    namespace render = foundation::render;

    // Build the CameraOverride a camera-preview renders through, from the target CameraComponent
    // and the entity's world matrix. Matches the runtime primary-camera extraction (Engine.Render
    // ExtractImpl): view = inverse(world), the projection from MakeCameraProjection, and the eye
    // position from the world origin. Aspect comes from the component (the preview panel is sized
    // to match, so the framing shown is the camera's real framing).
    [[nodiscard]] inline render::CameraOverride
    BuildCameraPreviewOverride(const engine::render::CameraComponent& camera, const Float4x4& world)
    {
        render::ViewCamera view;
        view.view = Inverse(world);
        view.projection = engine::render::MakeCameraProjection(camera, camera.aspect);
        view.position = TransformPoint(Float3{0, 0, 0}, world);
        view.farZ = camera.farZ;

        render::CameraOverride result;
        result.camera = view;
        result.clearColor = ToLinear(camera.clearColor); // authored sRGB -> render view
        return result;
    }

    // The camera preview's visibility and pin decision (pure, no UI). A pin remembers the entity:
    // while the pinned entity is alive and still a camera it wins over the selection. Otherwise the
    // pin is stale and must be cleared (unpin = true), and the preview shows the selection if it is
    // a camera. The caller resolves `selectionIsCamera` and `pinnedValidCamera` against the live
    // scene (entity alive and has a CameraComponent), so this decision stays testable.
    struct CameraPreviewResolution
    {
        scene::EntityHandle target; // the camera entity to preview (unassigned = none)
        bool visible = false;       // whether the preview should render this frame
        bool unpin = false;         // the caller should clear its pin (its pinned entity went stale)
    };

    [[nodiscard]] inline CameraPreviewResolution
    ResolveCameraPreview(scene::EntityHandle selection, bool selectionIsCamera,
                         scene::EntityHandle pinned, bool pinnedValidCamera)
    {
        if (pinned.IsAssigned() && pinnedValidCamera)
        {
            return CameraPreviewResolution{pinned, true, false};
        }
        // A pin that is set but no longer a live camera is stale; the caller is told to clear it.
        const bool unpin = pinned.IsAssigned();
        if (selection.IsAssigned() && selectionIsCamera)
        {
            return CameraPreviewResolution{selection, true, unpin};
        }
        return CameraPreviewResolution{scene::EntityHandle{}, false, unpin};
    }
}
