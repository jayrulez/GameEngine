// draconic.engine.particles:subsystem - the Context-level ParticleSubsystem.
//
// Mirrors AnimationSubsystem: injects the ParticleEffectComponentManager into every scene (so
// attaching a ParticleEffectComponent is all an app needs). Additionally it owns the dedicated
// ParticleRenderer and registers it + itself (as the render-data extractor / IRenderDataProvider)
// with RenderSubsystem via the generic seam - so NO particle code lives in RenderSubsystem. Its
// ExtractInto delegates to the scene's manager, which packs the live billboards.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.engine.particles:subsystem;

import draconic.foundation;
import draconic.rhi;
import draconic.shaders.system;
import draconic.runtime;          // Subsystem, Context
import draconic.scene;            // Scene, ISceneAware
import draconic.engine.scene;  // SceneSubsystem
import draconic.render;           // ExtractedScene
import draconic.engine.render; // RenderSubsystem + IRenderExtractor seam
import :renderer;
import :components;

using namespace draconic::foundation;
namespace rhi = draconic::rhi;
namespace shaders = draconic::shaders;
namespace scene = draconic::scene;
namespace render = draconic::render;

export namespace draconic::particles
{
    class ParticleSubsystem final : public draconic::runtime::Subsystem, public scene::ISceneAware
    {
    public:
        // Inject the particle manager into each new scene, then register it (as the scene's render-data
        // provider) and hand it the renderer's dispatch id so its billboards route correctly.
        void OnSceneCreated(scene::Scene& scene) override
        {
            EnsureRenderer();
            ParticleEffectComponentManager* mgr = scene.AddSystem<ParticleEffectComponentManager>();
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
            draconic::runtime::Context* ctx = GetContext();
            if (ctx == nullptr)
            {
                return;
            }
            if (auto* scenes = ctx->GetSubsystem<scene::SceneSubsystem>())
            {
                scenes->RegisterSceneAware(this);
            }
            m_render = ctx->GetSubsystem<render::RenderSubsystem>();
            EnsureRenderer(); // GPU systems are up by OnReady (RenderSubsystem::OnInit ran first)
        }

    private:
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

        render::RenderSubsystem* m_render = nullptr;
        UniquePtr<ParticleRenderer> m_renderer;
        u16 m_billboardRendererId = 0;
    };
}
