// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// engine.terrain:subsystem - the application-level TerrainSubsystem.
//
// Mirrors ParticleSubsystem: owns the dedicated TerrainRenderer and wires it into RenderSubsystem via
// the generic seam - registers the renderer (-> dispatch id), and per scene registers the
// TerrainComponentManager as the IRenderDataProvider + hands it the device + id. So NO terrain code
// lives in RenderSubsystem, and attaching a TerrainComponent is all an app needs. The manager itself
// is injected into every scene by scene composition (AddTerrainSceneManagers), not here.

module;
#include "Core/Prelude.h"

export module engine.terrain:subsystem;

import foundation.core;
import foundation.rhi;
import foundation.shaders.system;
import foundation.runtime; // Subsystem, Context
import foundation.scene;   // Scene, ISceneObserver
import engine.scene;       // SceneSubsystem
import foundation.render;  // ExtractedScene
import engine.render;      // RenderSubsystem + the register-renderer/provider seam
import :renderer;
import :components;

using namespace foundation::core;
namespace rhi = foundation::rhi;
namespace shaders = foundation::shaders;
namespace scene = foundation::scene;

export namespace engine::terrain
{
    namespace scene = foundation::scene;

    class TerrainSubsystem final : public foundation::runtime::Subsystem, public scene::ISceneObserver
    {
    public:
        void OnSystemsReady(scene::Scene& scene) override
        {
            EnsureRenderer();
            TerrainComponentManager* mgr = scene.GetSystem<TerrainComponentManager>();
            if (mgr == nullptr || m_render == nullptr)
            {
                return;
            }
            mgr->SetRenderContext(m_render->Device(), m_rendererId, m_render->RetireQueue());
            m_render->RegisterProvider(scene, *mgr);
            m_managers.PushBack(mgr); // tracked for GPU teardown (scene destroy + shutdown)
        }

        void OnDestroying(scene::Scene& scene) override
        {
            // The scene dies while the device is still alive - free its manager's GPU state
            // NOW (the manager's own destructor may run after the device is gone).
            TerrainComponentManager* mgr = scene.GetSystem<TerrainComponentManager>();
            if (mgr == nullptr)
            {
                return;
            }
            mgr->ClearGpu();
            for (usize i = 0; i < m_managers.Size(); ++i)
            {
                if (m_managers[i] == mgr)
                {
                    m_managers.RemoveAt(i);
                    break;
                }
            }
        }

    protected:
        void OnInit() override
        {
            RegisterTerrainComponentReflection(); // tooling: reflected components (idempotent)
        }

        void OnReady() override
        {
            foundation::runtime::SubsystemManager* ctx = Manager();
            if (ctx == nullptr)
            {
                return;
            }
            if (auto* scenes = ctx->GetSubsystem<engine::scene::SceneSubsystem>())
            {
                scenes->RegisterObserver(this, scene::SceneLifecycleStage::SystemsReady);
                scenes->RegisterObserver(this, scene::SceneLifecycleStage::Destroying);
            }
            m_render = ctx->GetSubsystem<engine::render::RenderSubsystem>();
            EnsureRenderer();
        }

    private:
        void OnPrepareShutdown() override
        {
            // Scenes still alive at shutdown clear their GPU state HERE, in the prepare phase:
            // every subsystem's PrepareShutdown runs before any Shutdown, so the caches' retired
            // textures land in the render subsystem's queue before it flushes. (Shutdown runs in
            // reverse UPDATE order - render, at 1000, shuts down first - so a clear in OnShutdown
            // retired into an already-flushed queue: the playground's three leaked images.)
            for (TerrainComponentManager* mgr : m_managers)
            {
                mgr->ClearGpu();
            }
            m_managers.Clear();
        }

        void OnShutdown() override
        {
            if (foundation::runtime::SubsystemManager* ctx = Manager())
            {
                if (auto* scenes = ctx->GetSubsystem<engine::scene::SceneSubsystem>())
                {
                    scenes->UnregisterObserver(this);
                }
            }
        }

        void EnsureRenderer()
        {
            if (m_renderer.Get() != nullptr || m_render == nullptr)
            {
                return;
            }
            rhi::Device* device = m_render->Device();
            shaders::ShaderSystem* sh = m_render->Shaders();
            if (device == nullptr || sh == nullptr)
            {
                return;
            }
            m_renderer = MakeUnique<TerrainRenderer>(DefaultAllocator(), *device, *sh,
                                                     m_render->FramesInFlight());
            if (m_renderer->Initialize().IsOk())
            {
                m_rendererId = m_render->RegisterRenderer(*m_renderer);
                m_renderer->SetRetireQueue(m_render->RetireQueue());
            }
            else
            {
                m_renderer.Reset();
            }
        }

        engine::render::RenderSubsystem* m_render = nullptr;
        Array<TerrainComponentManager*> m_managers; // borrowed; pruned on scene destroy
        UniquePtr<TerrainRenderer> m_renderer;
        u16 m_rendererId = 0;
    };
}
