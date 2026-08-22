/// Engine::Animation - the `:subsystem` partition.
///
/// AnimationSubsystem: a Context-level subsystem that registers the animation component reflection and
/// contributes the per-scene animation managers to the declarative scene composition (SceneModule). So
/// attaching a SkeletalAnimationComponent or AnimationGraphComponent is all an app needs - the scene's
/// per-scene tick then advances the players (PostUpdate) and feeds the skinning matrices to the mesh
/// components, before render extraction. The subsystem itself does no per-frame work; the managers
/// (SceneSystems) do, driven by the scene.

module;
#include "Core/Prelude.h"

export module engine.animation:subsystem;

import foundation.core;
import foundation.runtime;         // Subsystem, Context
import foundation.scene; // Scene
import engine.scene; // SceneSubsystem (to register as scene-aware)
import :components;
import :propertyanimator;

export namespace engine::animation
{
    // THE animation manager set for a scene - the subsystem injects it at runtime AND headless
    // scene consumers (Engine.SceneSurface -> export/MCP transcode scratch) call it directly, so
    // a manager added here reaches both automatically. Add a manager => bump the SceneSurface
    // tripwire (engine::kSceneSystemCount).
    inline void AddAnimationSceneManagers(foundation::scene::Scene& scene)
    {
        scene.AddSystem<AnimationGraphComponentManager>();
        scene.AddSystem<SkeletalAnimationComponentManager>();
        scene.AddSystem<InstancedSkinningComponentManager>(); // crowd skinning (shared pose pool)
        scene.AddSystem<PropertyAnimatorComponentManager>();  // reflected property-curve animation
    }

    class AnimationSubsystem final : public foundation::runtime::Subsystem
    {
    protected:
        void OnInit() override
        {
            RegisterAnimationComponentReflection(); // tooling: reflected components (idempotent)
        }
    };

} // namespace foundation::animation
