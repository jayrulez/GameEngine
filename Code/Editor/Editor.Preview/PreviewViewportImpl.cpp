// PreviewViewport implementation (see PreviewViewport.cppm). The bodies here are the
// substrate the bespoke pages used to each hand-roll verbatim: build a private preview
// scene, bind the ViewportView to its host window on first frame, drive the EditorCamera
// from the gated viewport devices, and render the scene through the real renderer with a
// CameraOverride into the viewport's color target.
//
// All the heavy render/scene/rhi/viewport/vg imports live HERE, behind Impl, so the module
// interface can be imported by heavy page TUs without the GCC module-merge ICE.

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
import foundation.ui;
import foundation.ui.runtime;
import foundation.ui.viewport;
import foundation.vg.renderer;
import editor.camera;

using namespace foundation::core;

namespace editor
{
    namespace rhi = foundation::rhi;
    namespace render = foundation::render;
    namespace vg = foundation::vg;

    struct PreviewViewport::Impl
    {
        runtime::IApplicationHost* host = nullptr;
        ui::runtime::UIHost* uiHost = nullptr;
        engine::scene::SceneSubsystem* scenes = nullptr;
        scene::SceneManager sceneManager; // this preview's OWN scene group
        engine::render::RenderSubsystem* render = nullptr;
        scene::Scene* scene = nullptr;
        EditorCamera camera;
        UniquePtr<foundation::shell::InputRouter> router;
        RefPtr<ui::viewport::ViewportView> viewport;
        foundation::graphics::RenderWindow* hostWindow = nullptr;

        void EnsureViewportBound();
    };

    PreviewViewport::PreviewViewport(runtime::IApplicationHost& host, ui::runtime::UIHost& uiHost,
                                     StringView sceneName)
        : m_impl(MakeUnique<Impl>(DefaultAllocator()))
    {
        m_impl->host = &host;
        m_impl->uiHost = &uiHost;
        m_impl->router =
            MakeUnique<foundation::shell::InputRouter>(DefaultAllocator(), host.Shell()->Input());

        m_impl->scenes = host.Ctx().GetSubsystem<engine::scene::SceneSubsystem>();
        m_impl->render = host.Ctx().GetSubsystem<engine::render::RenderSubsystem>();

        if (m_impl->scenes != nullptr)
        {
            m_impl->scenes->RegisterManager(&m_impl->sceneManager);
            m_impl->scene = m_impl->sceneManager.CreateScene(sceneName);
            m_impl->scene->SetSimulationEnabled(false);
        }

        m_impl->viewport = MakeRef<ui::viewport::ViewportView>(DefaultAllocator());
        m_impl->viewport->ClearColor = rhi::ClearColor{0.10f, 0.11f, 0.13f, 1.0f};
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

    ui::View* PreviewViewport::View() const
    {
        return m_impl->viewport.Get();
    }

    EditorCamera& PreviewViewport::Camera()
    {
        return m_impl->camera;
    }

    render::debug::DebugDraw& PreviewViewport::SceneDebugDraw()
    {
        return m_impl->render->DebugScene(*m_impl->scene);
    }

    void PreviewViewport::SetClearColor(Color color)
    {
        m_impl->viewport->ClearColor = rhi::ClearColor{color.r, color.g, color.b, color.a};
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

    void PreviewViewport::Update(f32 dt)
    {
        m_impl->EnsureViewportBound();
        if (m_impl->hostWindow == nullptr)
        {
            return;
        }
        m_impl->viewport->SyncInputRegion();
        if (m_impl->router)
        {
            m_impl->router->Update();
        }
        if (m_impl->viewport->IsHovered() || m_impl->viewport->IsFocused())
        {
            m_impl->camera.Update(m_impl->viewport->Keyboard(), m_impl->viewport->Mouse(), dt);
        }
    }

    void PreviewViewport::RenderFrame(foundation::graphics::FrameContext& frame)
    {
        ui::viewport::ViewportView* vp = m_impl->viewport.Get();
        if (vp == nullptr || !vp->IsReady() || !frame.valid)
        {
            return;
        }
        if (m_impl->render == nullptr || !m_impl->render->IsReady() || m_impl->scene == nullptr)
        {
            return;
        }
        const u32 w = vp->RenderWidth();
        const u32 h = vp->RenderHeight();
        if (w == 0 || h == 0 || !vp->IsEffectivelyVisible())
        {
            return;
        }

        const EditorCamera& cam = m_impl->camera;
        render::ViewCamera camera;
        camera.view = Float4x4::LookAtRH(cam.position, cam.position + cam.Forward(), cam.Up());
        camera.projection = Float4x4::PerspectiveFovRH(
            1.0472f, static_cast<f32>(w) / static_cast<f32>(h), 0.05f, 500.0f);
        camera.position = cam.position;
        camera.farZ = 500.0f;

        render::CameraOverride cameraOverride;
        cameraOverride.camera = camera;
        cameraOverride.clearColor = Color{vp->ClearColor.r, vp->ClearColor.g, vp->ClearColor.b,
                                          vp->ClearColor.a};

        render::TargetState targetState;
        targetState.texture = vp->ColorTexture();
        targetState.currentState = vp->ColorState();
        targetState.finalState = rhi::ResourceState::ShaderRead;

        m_impl->render->RenderScene(*m_impl->scene, vp->ColorTargetView(), vp->ColorFormat(), w, h,
                                    render::ViewportRect{0, 0, w, h}, &cameraOverride, targetState);
        vp->SetColorState(rhi::ResourceState::ShaderRead);
    }

    void PreviewViewport::Shutdown()
    {
        if (m_impl->viewport.Get() != nullptr)
        {
            m_impl->viewport->Shutdown();
        }
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

    void PreviewViewport::Impl::EnsureViewportBound()
    {
        foundation::ui::RootView* root = viewport->Root();
        if (root == nullptr)
        {
            return;
        }
        foundation::graphics::RenderWindow* window = uiHost->WindowForRoot(root);
        if (window == nullptr || window == hostWindow)
        {
            return;
        }
        vg::renderer::VGRenderer* renderer = uiHost->RendererFor(window);
        if (renderer == nullptr)
        {
            return;
        }
        if (hostWindow == nullptr)
        {
            viewport->Initialize(host->Graphics()->Raw(), renderer, host->Shell()->Input(),
                                 window->Window().Id());
            if (viewport->Surface() != nullptr)
            {
                router->AddSurface(viewport->Surface());
            }
        }
        else
        {
            viewport->AttachToWindow(renderer, window->Window().Id());
        }
        hostWindow = window;
    }
}
