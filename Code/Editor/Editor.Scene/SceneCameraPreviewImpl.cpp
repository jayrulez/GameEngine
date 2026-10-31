// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Scene - :scene_camera_preview partition, its body. See SceneCameraPreview.cppm.

module;
#include "Core/Prelude.h"

module editor.scene;

import foundation.core;
import foundation.scene;
import foundation.graphics;
import foundation.rhi;
import foundation.render.api;
import foundation.gui;
import foundation.gui.hosting;
import foundation.gui.viewport;
import engine.render;
import editor.core;
import :edit;
import :camera_preview;
import :scene_camera_preview;

using namespace foundation::core;
namespace gui = foundation::gui;
namespace scene = foundation::scene;

namespace editor
{
    namespace
    {
        template <typename T>
        T* FindIn(gui::Node& node, StringView id)
        {
            if (auto* widget = Cast<gui::UIWidget>(&node); widget != nullptr && widget->GetId() == id)
                return Cast<T>(&node);
            for (usize i = 0; i < node.ChildCount(); ++i)
                if (T* found = FindIn<T>(*node.GetChildAt(i), id))
                    return found;
            return nullptr;
        }
    }

    SceneCameraPreview::SceneCameraPreview(IAllocator& allocator, Services services, gui::Node& pane) : m_services(services)
    {
        m_view = MakeRef<gui::Viewport>(allocator);
        m_view->AddClass(u8"camera-preview-picture");
        m_view->ClearColor = Color{0.0f, 0.0f, 0.0f, 1.0f};
        m_view->SetTabFocusable(false);
        m_view->SetHitTestVisible(false); // the main view keeps the pointer under it
        m_panel = FindIn<gui::Node>(pane, u8"camera-preview");
        m_icon = FindIn<gui::Image>(pane, u8"camera-preview-icon");
        m_name = FindIn<gui::Label>(pane, u8"camera-preview-name");
        m_pin = FindIn<gui::Button>(pane, u8"camera-preview-pin");
        if (gui::Node* holder = FindIn<gui::Node>(pane, u8"camera-preview-view"))
            holder->AddChild(m_view.Get());
        if (m_panel != nullptr)
            m_panel->SetDisplayed(false); // until a camera is shown
        SceneCameraPreview* self = this;
        if (m_pin != nullptr)
            m_pin->SetOnClick([self]() { self->TogglePin(); });
    }

    void SceneCameraPreview::HostIn(gui::GuiHost* host)
    {
        if (host != nullptr)
            m_view->HostIn(*host);
    }

    engine::render::CameraComponent* SceneCameraPreview::CameraOf(const Guid& entity) const
    {
        if (m_services.edit == nullptr || entity.IsNil())
            return nullptr;
        scene::Scene& sc = m_services.edit->Scene();
        const scene::EntityHandle e = sc.FindEntity(entity);
        auto* cameras = sc.GetSystem<engine::render::CameraComponentManager>();
        return e.IsAssigned() && sc.IsValid(e) && cameras != nullptr ? cameras->Get(e) : nullptr;
    }
    bool SceneCameraPreview::IsCamera(const Guid& entity) const { return CameraOf(entity) != nullptr; }

    void SceneCameraPreview::Update()
    {
        if (m_services.edit == nullptr)
            return;
        scene::Scene& sc = m_services.edit->Scene();
        const Guid* primary = m_services.edit->EntitySelection().Primary();
        const Guid selected = primary != nullptr && IsCamera(*primary) ? *primary : Guid{};
        // :camera_preview's decision, over the live scene: a pin wins while its entity is alive and
        // still a camera, else the selection when it is a camera. The pin is kept by guid (a handle
        // does not outlive a reload), so a stale one (its entity gone, or no longer a camera) is
        // cleared here and the decision sees no pin.
        if (!IsCamera(m_pinned))
            m_pinned = Guid{};
        const scene::EntityHandle selection = selected.IsNil() ? scene::EntityHandle{} : sc.FindEntity(selected);
        const scene::EntityHandle pinned = m_pinned.IsNil() ? scene::EntityHandle{} : sc.FindEntity(m_pinned);
        const CameraPreviewResolution resolved = ResolveCameraPreview(selection, selection.IsAssigned(), pinned, pinned.IsAssigned());
        m_target = resolved.visible ? sc.GetEntityId(resolved.target) : Guid{};

        if (m_panel != nullptr && m_panel->IsDisplayed() != !m_target.IsNil())
            m_panel->SetDisplayed(!m_target.IsNil());
        if (m_target.IsNil())
            return;
        // Titled with its camera's name and icon.
        const StringView name = sc.GetEntityName(sc.FindEntity(m_target));
        if (m_name != nullptr)
            m_name->SetText(name.IsEmpty() ? StringView(u8"(unnamed)") : name);
        if (m_icon != nullptr && m_services.icons != nullptr)
        {
            const TypeIcon icon = m_services.icons->For(TypeOf<engine::render::CameraComponent>(), StringView{});
            m_icon->SetSource(icon.Source);
            if (!icon.Class.IsEmpty() && !m_icon->HasClass(icon.Class.AsView()))
                m_icon->AddClass(icon.Class.AsView());
        }
        if (m_pin != nullptr)
        {
            const bool on = !m_pinned.IsNil() && m_pinned == m_target;
            if (on != m_pin->HasClass(u8"checked"))
            {
                if (on)
                    m_pin->AddClass(u8"checked");
                else
                    m_pin->RemoveClass(u8"checked");
            }
            m_pin->SetTooltip(on ? StringView(u8"Unpin: show the selected camera again") : StringView(u8"Pin: keep showing this camera"));
        }
        // At the camera's aspect, clamped to something sensible (a thin strip or a tower helps no
        // one).
        const engine::render::CameraComponent* camera = CameraOf(m_target);
        const f32 aspect = Clamp(camera != nullptr && camera->aspect > 0.0f ? camera->aspect : 16.0f / 9.0f, 0.5f, 3.0f);
        const Float2 size{kPictureWidth, Round(kPictureWidth / aspect)};
        if (size.x != m_size.x || size.y != m_size.y)
        {
            m_size = size;
            m_view->EditLayoutStyle([size](gui::LayoutStyle& s)
                                    {
                                        s.Width = gui::LayoutLength::Px(size.x);
                                        s.Height = gui::LayoutLength::Px(size.y);
                                    });
        }
    }

    void SceneCameraPreview::TogglePin()
    {
        if (!m_pinned.IsNil() && m_pinned == m_target)
            m_pinned = Guid{};
        else if (!m_target.IsNil())
            m_pinned = m_target;
        Update();
    }

    bool SceneCameraPreview::IsShown() const { return m_panel != nullptr && m_panel->IsDisplayed() && !m_target.IsNil(); }
    StringView SceneCameraPreview::Title() const { return m_name != nullptr && IsShown() ? m_name->GetText() : StringView{}; }

    void SceneCameraPreview::Render(foundation::graphics::FrameContext& frame)
    {
        engine::render::RenderSubsystem* render = m_services.render;
        if (!frame.valid || render == nullptr || !render->IsReady() || m_services.edit == nullptr || m_target.IsNil())
            return;
        if (!m_view->IsReady() || !m_view->IsTreeVisible() || m_view->RenderWidth() == 0 || m_view->RenderHeight() == 0)
            return;
        const engine::render::CameraComponent* camera = CameraOf(m_target);
        if (camera == nullptr)
            return;
        scene::Scene& sc = m_services.edit->Scene();
        const u32 w = m_view->RenderWidth();
        const u32 h = m_view->RenderHeight();
        foundation::render::CameraOverride cameraOverride = BuildCameraPreviewOverride(*camera, sc.GetWorldMatrix(sc.FindEntity(m_target)));
        foundation::render::TargetState target;
        target.texture = m_view->ColorTexture();
        target.currentState = m_view->ColorState();
        target.finalState = foundation::rhi::ResourceState::ShaderRead;
        // Keyed by the preview's own viewport: its debug list is empty, so the editor's overlays
        // (drawn into the main view's) never show here, and no shading override applies, so it
        // shows the game's look.
        render->RenderScene(sc, m_view->ColorView(), m_view->ColorFormat(), w, h, foundation::render::ViewportRect{0, 0, w, h}, &cameraOverride, target,
                            nullptr, m_view.Get(), nullptr);
        m_view->SetColorState(foundation::rhi::ResourceState::ShaderRead);
    }
}
