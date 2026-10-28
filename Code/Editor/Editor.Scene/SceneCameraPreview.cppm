// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Scene - :scene_camera_preview partition.
//
// The camera preview in the HUD's bottom end (Documentation/Specs/editor-gui.md, 5.3, layer 6;
// the mock's scene.xml): what a scene camera sees while the scene is edited from elsewhere. It
// shows the selected camera, or the one pinned (a pin wins while its entity is alive and still a
// camera; a stale pin clears itself, :camera_preview's decision), titled with that camera's name
// and icon, its picture at the camera's own aspect. The picture is a second view of the scene
// through the camera, keyed by the preview's own viewport, so the editor's grid, gizmos and
// markers (the main view's debug list) never show in it, nor the main view's shading override.
// It takes no input: the main view keeps the pointer under it.

module;
#include "Core/Prelude.h"

export module editor.scene:scene_camera_preview;

import foundation.core;
import foundation.scene;
import foundation.graphics;
import foundation.gui;
import foundation.gui.hosting;
import foundation.gui.viewport;
import engine.render;
import editor.core;
import :edit;

using namespace foundation::core;

export namespace editor
{
    /// SceneCameraPreview::kPictureWidth, at namespace scope for another library to read (MSVC exports no class's
    /// static constexpr member from a shared library).
    inline constexpr f32 kSceneCameraPreviewPictureWidth = 194.0f;

    class SceneCameraPreview
    {
    public:
        /// The picture's width: the panel's 200 px less its padding (editor.css, .camera-preview).
        static constexpr f32 kPictureWidth = kSceneCameraPreviewPictureWidth;

        struct Services
        {
            SceneEditContext* edit = nullptr;
            EditorIcons* icons = nullptr;                      // the camera's icon (null: the generic one)
            engine::render::RenderSubsystem* render = nullptr; // null: nothing renders (a test)
        };

        /// The preview over `edit`'s scene in `pane` (the markup's `camera-preview`,
        /// `camera-preview-view`, `camera-preview-icon`, `camera-preview-name` and
        /// `camera-preview-pin`, by id).
        SceneCameraPreview(IAllocator& allocator, Services services, foundation::gui::Node& pane);
        SceneCameraPreview(const SceneCameraPreview&) = delete;
        SceneCameraPreview& operator=(const SceneCameraPreview&) = delete;

        /// Drawn by `host` from now on (null: not hosted, a test).
        void HostIn(foundation::gui::GuiHost* host);
        /// Once a frame: which camera, its title and aspect, shown or hidden.
        void Update();
        /// This frame's picture (inside the renderer's bracket): nothing while hidden.
        void Render(foundation::graphics::FrameContext& frame);
        /// Pins the camera shown, or unpins it when it is the pinned one.
        void TogglePin();

        // === What it shows (for tests) ===
        [[nodiscard]] bool IsShown() const;
        [[nodiscard]] const Guid& Target() const noexcept { return m_target; }
        [[nodiscard]] const Guid& Pinned() const noexcept { return m_pinned; }
        [[nodiscard]] StringView Title() const;
        /// The picture's size in the layout's pixels.
        [[nodiscard]] Float2 PictureSize() const noexcept { return m_size; }
        [[nodiscard]] foundation::gui::Viewport& Picture() noexcept { return *m_view; }

    private:
        [[nodiscard]] bool IsCamera(const Guid& entity) const;
        [[nodiscard]] engine::render::CameraComponent* CameraOf(const Guid& entity) const;

        Services m_services;
        RefPtr<foundation::gui::Viewport> m_view;
        foundation::gui::Node* m_panel = nullptr;
        foundation::gui::Image* m_icon = nullptr;
        foundation::gui::Label* m_name = nullptr;
        foundation::gui::Button* m_pin = nullptr;
        Guid m_target; // the camera shown (nil: none)
        Guid m_pinned; // the camera pinned (nil: the selection's)
        Float2 m_size;
    };
}
