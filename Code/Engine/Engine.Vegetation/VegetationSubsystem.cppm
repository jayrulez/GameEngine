// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// engine.vegetation:subsystem - the Context-level VegetationSubsystem.
//
// The TerrainSubsystem pattern without a renderer of its own: per scene it registers the
// TerrainVegetationComponentManager (injected by scene composition) as an IRenderDataProvider on
// RenderSubsystem, so its instanced sets ride the shared MeshRenderer. No GPU state lives in the
// manager (the renderer owns the instance buffers and evicts them), so nothing tears down here.

module;
#include "Core/Prelude.h"

export module engine.vegetation:subsystem;

import foundation.core;
import foundation.runtime; // Subsystem, Context
import foundation.scene;   // Scene, ISceneObserver
import engine.scene;       // SceneSubsystem
import engine.render;      // RenderSubsystem + the register-provider seam
import :components;

using namespace foundation::core;

export namespace engine::vegetation
{
    namespace scene = foundation::scene;

    class VegetationSubsystem final : public foundation::runtime::Subsystem,
                                      public scene::ISceneObserver
    {
    public:
        void OnSystemsReady(scene::Scene& scene) override
        {
            auto* mgr = scene.GetSystem<TerrainVegetationComponentManager>();
            if (mgr == nullptr || m_render == nullptr)
            {
                return;
            }
            m_render->RegisterProvider(scene, *mgr);
        }

    protected:
        void OnInit() override
        {
            RegisterVegetationComponentReflection(); // tooling: reflected components (idempotent)
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
            }
            m_render = ctx->GetSubsystem<engine::render::RenderSubsystem>();
        }

    private:
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

        engine::render::RenderSubsystem* m_render = nullptr;
    };
}
