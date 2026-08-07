/// Draconic::AnimationSubsystem - the `:subsystem` partition.
///
/// AnimationSubsystem: a Context-level subsystem that injects the animation component managers
/// (skeletal single-clip + animation-graph) into every scene (via ISceneAware), so attaching a
/// SkeletalAnimationComponent or AnimationGraphComponent is all an app needs - the SceneSubsystem's
/// per-scene tick then advances the players (PostUpdate) and feeds the skinning matrices to the mesh
/// components, before render extraction. The subsystem itself does no per-frame work; the managers
/// (SceneSystems) do, driven by the scene.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.engine.animation:subsystem;

import draconic.foundation;
import draconic.runtime;         // Subsystem, Context
import draconic.scene;           // Scene, ISceneAware
import draconic.engine.scene; // SceneSubsystem (to register as scene-aware)
import :components;

export namespace draconic::animation
{

    class AnimationSubsystem final : public draconic::runtime::Subsystem,
                                     public draconic::scene::ISceneAware
    {
    public:
        // Injects the animation managers into each new scene (they tick in PostUpdate): the graph
        // manager (state machines / blend trees) runs first, then the simple single-clip manager.
        void OnSceneCreated(draconic::scene::Scene& scene) override
        {
            scene.AddSystem<AnimationGraphComponentManager>();
            scene.AddSystem<SkeletalAnimationComponentManager>();
            scene.AddSystem<InstancedSkinningComponentManager>(); // crowd skinning (shared pose pool)
        }

    protected:
        void OnInit() override
        {
            RegisterAnimationComponentReflection(); // tooling: reflected components (idempotent)
        }

        void OnReady() override
        {
            if (draconic::runtime::Context* ctx = GetContext())
            {
                if (auto* scenes = ctx->GetSubsystem<draconic::scene::SceneSubsystem>())
                {
                    scenes->RegisterSceneAware(this);
                }
            }
        }

        void OnShutdown() override
        {
            if (draconic::runtime::Context* ctx = GetContext())
            {
                if (auto* scenes = ctx->GetSubsystem<draconic::scene::SceneSubsystem>())
                {
                    scenes->UnregisterSceneAware(this);
                }
            }
        }
    };

} // namespace draconic::animation
