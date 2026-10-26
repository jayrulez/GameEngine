// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// PreviewViewport implementation (see PreviewViewport.cppm). The bodies here are the
// substrate each bespoke page would otherwise hand-roll verbatim: build a private preview
// scene, drive the EditorCamera from the viewport's own input, and render the scene through
// the real renderer with a CameraOverride into the viewport's color target.
//
// All the heavy render/scene/rhi imports live HERE, behind Impl, so the module interface can be
// imported by heavy page TUs without the GCC module-merge ICE.

module;
#include "Core/Prelude.h"

module editor.preview;

import foundation.core;
import foundation.shell;
import foundation.runtime;
import foundation.runtime.client;
import foundation.graphics;
import foundation.rhi;
import foundation.scene;
import engine.scene;
import foundation.render;
import engine.render;
import foundation.gui;
import foundation.gui.hosting;
import foundation.gui.viewport;
import editor.camera;

using namespace foundation::core;

namespace editor
{
    namespace rhi = foundation::rhi;
    namespace render = foundation::render;
    namespace gui = foundation::gui;

    struct PreviewViewport::Impl
    {
        engine::scene::SceneSubsystem* scenes = nullptr;
        scene::SceneManager sceneManager{editor::EditorRootAllocator()}; // this preview's OWN scene group
        engine::render::RenderSubsystem* render = nullptr;
        scene::Scene* scene = nullptr;
        EditorCamera camera = PreviewCamera();
        RefPtr<gui::Viewport> view;

        // A preview looks at one asset close up: a nearer near plane and a shorter reach than
        // the scene view's.
        static EditorCamera PreviewCamera()
        {
            EditorCamera c;
            c.nearZ = 0.05f;
            c.farZ = 500.0f;
            return c;
        }

        // Its content has the input: hovered, focused, or holding a press (a look drag, its
        // pointer locked and hidden, may drift off it).
        [[nodiscard]] bool IsActive() const
        {
            const foundation::shell::InputSurface* surface = view->HostedSurface();
            return view->IsHovered() || view->IsFocused() || (surface != nullptr && surface->Captured());
        }
        // The keyboard while it has the focus or holds a press; null otherwise.
        [[nodiscard]] foundation::shell::IKeyboard* Keyboard() const
        {
            const foundation::shell::InputSurface* surface = view->HostedSurface();
            return view->IsFocused() || (surface != nullptr && surface->Captured()) ? view->Keyboard() : nullptr;
        }
    };

    PreviewViewport::PreviewViewport(runtime::IApplicationHost& host, gui::GuiHost* gui, StringView sceneName)
        : m_impl(MakeUnique<Impl>(editor::EditorRootAllocator()))
    {
        m_impl->scenes = host.Ctx().GetSubsystem<engine::scene::SceneSubsystem>();
        m_impl->render = host.Ctx().GetSubsystem<engine::render::RenderSubsystem>();
        if (m_impl->scenes != nullptr)
        {
            m_impl->scenes->RegisterManager(&m_impl->sceneManager);
            m_impl->scene = m_impl->sceneManager.CreateScene(sceneName);
            m_impl->scene->SetSimulationEnabled(false);
        }
        m_impl->view = MakeRef<gui::Viewport>(editor::EditorRootAllocator());
        m_impl->view->AddClass(u8"preview-view");
        m_impl->view->ClearColor = kBackdrop;
        if (gui != nullptr)
            m_impl->view->HostIn(*gui);
    }

    PreviewViewport::~PreviewViewport()
    {
        Shutdown();
    }

    bool PreviewViewport::IsValid() const
    {
        return m_impl->scene != nullptr;
    }

    scene::Scene* PreviewViewport::Scene() const
    {
        return m_impl->scene;
    }

    scene::Scene* PreviewViewport::ResetScene()
    {
        if (m_impl->scenes == nullptr)
        {
            return nullptr;
        }
        String name(m_impl->scene != nullptr ? m_impl->scene->Name() : StringView(u8"preview"));
        if (m_impl->scene != nullptr)
        {
            m_impl->sceneManager.DestroyScene(m_impl->scene);
        }
        m_impl->scene = m_impl->sceneManager.CreateScene(name.AsView());
        m_impl->scene->SetSimulationEnabled(false);
        return m_impl->scene;
    }

    RefPtr<gui::Node> PreviewViewport::View() const
    {
        return RefPtr<gui::Node>(m_impl->view.Get());
    }

    EditorCamera& PreviewViewport::Camera()
    {
        return m_impl->camera;
    }

    void PreviewViewport::Frame(Float3 center, f32 radius)
    {
        m_impl->camera.FrameBounds(center, radius);
    }

    render::debug::DebugDraw& PreviewViewport::SceneDebugDraw()
    {
        return m_impl->render->DebugScene(*m_impl->scene);
    }

    void PreviewViewport::SetClearColor(Color color)
    {
        m_impl->view->ClearColor = color;
    }

    void PreviewViewport::SetSimulationEnabled(bool enabled)
    {
        if (m_impl->scene != nullptr)
        {
            m_impl->scene->SetSimulationEnabled(enabled);
        }
    }

    void PreviewViewport::SetTimeScale(f32 scale)
    {
        m_impl->sceneManager.SetTimeScale(scale);
    }

    bool PreviewViewport::Update(f32 dt)
    {
        gui::Viewport& view = *m_impl->view;
        foundation::shell::InputSurface* surface = view.HostedSurface();
        bool frame = false;
        if (m_impl->IsActive())
        {
            foundation::shell::IKeyboard* keyboard = m_impl->Keyboard();
            m_impl->camera.Update(keyboard, view.Mouse(), dt);
            // An orbit, look or pan drag: the pointer locked meanwhile, ended with the press.
            if (surface != nullptr)
                surface->SetPointerLock(m_impl->camera.WantsPointerLock());
            // F frames, as the scene view's does (not while the camera flies on WASD).
            frame = keyboard != nullptr && keyboard->IsKeyPressed(foundation::shell::KeyCode::F) && !m_impl->camera.WantsPointerLock();
        }
        else
        {
            // Losing the input releases a captured fly at once.
            m_impl->camera.ReleaseCapture(view.Mouse());
            if (surface != nullptr)
                surface->SetPointerLock(false);
        }
        m_impl->camera.Advance(dt); // a framing glides on either way
        return frame;
    }

    void PreviewViewport::RenderFrame(foundation::graphics::FrameContext& frame)
    {
        gui::Viewport& view = *m_impl->view;
        if (!frame.valid || m_impl->render == nullptr || !m_impl->render->IsReady() || m_impl->scene == nullptr)
        {
            return;
        }
        if (!view.IsReady() || !view.IsTreeVisible() || view.RenderWidth() == 0 || view.RenderHeight() == 0)
        {
            return; // hidden (a tab not in front) or not placed yet
        }
        const u32 w = view.RenderWidth();
        const u32 h = view.RenderHeight();

        const EditorCamera& cam = m_impl->camera;
        render::ViewCamera camera;
        camera.view = Float4x4::LookAtRH(cam.position, cam.position + cam.Forward(), cam.Up());
        camera.projection = cam.Projection(static_cast<f32>(w) / static_cast<f32>(h));
        camera.position = cam.position;
        camera.farZ = cam.farZ;

        render::CameraOverride cameraOverride;
        cameraOverride.camera = camera;
        // The viewport's colour is a UI colour (sRGB); the render view takes linear.
        const rhi::ClearColor clear = view.LinearClearColor();
        cameraOverride.clearColor = Color{clear.r, clear.g, clear.b, clear.a};

        // The graph imports the colour target and owns its transitions: from the viewport's state
        // to ShaderRead for the GUI to sample.
        render::TargetState targetState;
        targetState.texture = view.ColorTexture();
        targetState.currentState = view.ColorState();
        targetState.finalState = rhi::ResourceState::ShaderRead;

        m_impl->render->RenderScene(*m_impl->scene, view.ColorView(), view.ColorFormat(), w, h,
                                    render::ViewportRect{0, 0, w, h}, &cameraOverride, targetState);
        view.SetColorState(rhi::ResourceState::ShaderRead);
    }

    void PreviewViewport::Shutdown()
    {
        if (m_impl->view)
            m_impl->camera.ReleaseCapture(m_impl->view->Mouse());
        if (m_impl->scene != nullptr)
        {
            m_impl->sceneManager.DestroyScene(m_impl->scene);
            m_impl->scene = nullptr;
        }
        if (m_impl->scenes != nullptr)
        {
            m_impl->scenes->UnregisterManager(&m_impl->sceneManager);
            m_impl->scenes = nullptr;
        }
    }
}
