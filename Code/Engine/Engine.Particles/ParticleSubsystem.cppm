// engine.particles:subsystem - the Context-level ParticleSubsystem.
//
// Mirrors AnimationSubsystem: injects the ParticleEffectComponentManager into every scene (so
// attaching a ParticleEffectComponent is all an app needs). Additionally it owns the dedicated
// ParticleRenderer and registers it + itself (as the render-data extractor / IRenderDataProvider)
// with RenderSubsystem via the generic seam - so NO particle code lives in RenderSubsystem. Its
// ExtractInto delegates to the scene's manager, which packs the live billboards.

module;
#include "Core/Prelude.h"

export module engine.particles:subsystem;

import foundation.core;
import foundation.rhi;
import foundation.shaders.system;
import foundation.runtime;          // Subsystem, Context
import foundation.scene; // Scene
import engine.scene;  // SceneSubsystem
import foundation.render;           // ExtractedScene
import engine.render; // RenderSubsystem + IRenderExtractor seam
import :renderer;
import :components;

using namespace foundation::core;
namespace rhi = foundation::rhi;
namespace shaders = foundation::shaders;
namespace scene = foundation::scene;

export namespace engine::particles
{
    // Foundation aliases (sibling engine::* namespaces would otherwise shadow these).
    namespace scene = foundation::scene;

    // THE particle manager set for a scene - injected by the subsystem at runtime AND by headless
    // scene consumers (Engine.SceneSurface). Renderer wiring (dispatch id, provider registration)
    // stays with the subsystem. Add a manager => bump the SceneSurface tripwire
    // (engine::kSceneSystemCount).
    inline void AddParticleSceneManagers(scene::Scene& scene)
    {
        scene.AddSystem<ParticleEffectComponentManager>();
    }

    class ParticleSubsystem final : public foundation::runtime::Subsystem, public scene::ISceneObserver
    {
    public:
        void OnSystemsReady(scene::Scene& scene) override
        {
            EnsureRenderer();
            ParticleEffectComponentManager* mgr = scene.GetSystem<ParticleEffectComponentManager>();
            if (mgr == nullptr)
            {
                return;
            }
            mgr->SetBillboardRendererId(m_billboardRendererId);
            if (m_render != nullptr)
            {
                m_render->RegisterProvider(scene, *mgr);
            }
        }

    protected:
        void OnInit() override
        {
            RegisterParticleComponentReflection(); // tooling: reflected components (idempotent)
        }

        void OnReady() override
        {
            foundation::runtime::Context* ctx = GetContext();
            if (ctx == nullptr)
            {
                return;
            }
            if (auto* scenes = ctx->GetSubsystem<engine::scene::SceneSubsystem>())
            {
                scenes->RegisterObserver(this, scene::SceneLifecycleStage::SystemsReady);
            }
            m_render = ctx->GetSubsystem<engine::render::RenderSubsystem>();
            EnsureRenderer(); // GPU systems are up by OnReady (RenderSubsystem::OnInit ran first)
        }

    private:
        void OnShutdown() override
        {
            if (foundation::runtime::Context* ctx = GetContext())
            {
                if (auto* scenes = ctx->GetSubsystem<engine::scene::SceneSubsystem>())
                {
                    scenes->UnregisterObserver(this);
                }
            }
        }

        // Create + register the ParticleRenderer once the render GPU systems exist (idempotent).
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
            m_renderer = MakeUnique<ParticleRenderer>(DefaultAllocator(), *device, *sh,
                                                      m_render->FramesInFlight());
            if (m_renderer->Initialize().IsOk())
            {
                m_billboardRendererId = m_render->RegisterRenderer(*m_renderer);
                m_renderer->SetRetireQueue(m_render->RetireQueue()); // web-safe ring grows
            }
            else
            {
                m_renderer.Reset();
            }
        }

        engine::render::RenderSubsystem* m_render = nullptr;
        UniquePtr<ParticleRenderer> m_renderer;
        u16 m_billboardRendererId = 0;
    };
}
