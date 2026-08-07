// Draconic::Editor.Scene - :camera_preview partition (task #118).
//
// The testable, UI-FREE core of the scene-editor camera preview: the CameraOverride a selected /
// pinned CameraComponent renders through, and the visibility/pin DECISION. The preview view class
// and its ScenePage wiring live in the impl units; these pure functions carry the logic so it can
// be unit-tested without a renderer or a UI tree (per the spec).

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.editor.scene:camera_preview;

import draconic.foundation;
import draconic.scene;
import draconic.render.api;    // ViewCamera / CameraOverride
import draconic.engine.render; // CameraComponent

using namespace draconic::foundation;

export namespace draconic::editor
{
    namespace scene = draconic::scene;
    namespace render = draconic::render;

    // Build the CameraOverride a camera-preview renders through, from the target CameraComponent and
    // the entity's WORLD matrix. Matches the runtime primary-camera extraction (Engine.Render
    // ExtractImpl): view = inverse(world), a right-handed perspective from the lens fields, and the
    // eye position from the world origin. Aspect comes from the component (the preview panel is
    // sized to match, so the framing shown is the camera's real framing).
    [[nodiscard]] inline render::CameraOverride
    BuildCameraPreviewOverride(const render::CameraComponent& camera, const Float4x4& world)
    {
        render::ViewCamera view;
        view.view = Inverse(world);
        view.projection =
            Float4x4::PerspectiveFovRH(camera.fovYRadians, camera.aspect, camera.nearZ, camera.farZ);
        view.position = TransformPoint(Float3{0, 0, 0}, world);
        view.farZ = camera.farZ;

        render::CameraOverride result;
        result.camera = view;
        result.clearColor = camera.clearColor;
        return result;
    }

    // The camera-preview visibility/pin decision (UI-free, pure). A pin remembers the ENTITY: while
    // the pinned entity is alive AND still a camera it wins over the selection; otherwise the pin is
    // STALE and must be cleared (unpin = true), falling back to "show the selection iff it is a
    // camera". `selectionIsCamera` / `pinnedValidCamera` are resolved by the caller against the live
    // scene (entity alive + has a CameraComponent), so this decision stays testable.
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
        // A pin that is set but no longer a live camera is stale -> tell the caller to clear it.
        const bool unpin = pinned.IsAssigned();
        if (selection.IsAssigned() && selectionIsCamera)
        {
            return CameraPreviewResolution{selection, true, unpin};
        }
        return CameraPreviewResolution{scene::EntityHandle{}, false, unpin};
    }
}
