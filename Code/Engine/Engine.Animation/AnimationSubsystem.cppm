// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Engine::Animation - the `:subsystem` partition.
///
/// AnimationSubsystem: an application-level subsystem that registers the animation component
/// reflection and contributes the per-scene animation managers to the declarative scene
/// composition (SceneModule). Attaching a SkeletalAnimationComponent or AnimationGraphComponent is
/// all an app needs: the scene's tick advances the players (PostUpdate) and feeds the skinning
/// matrices to the mesh components before render extraction. The subsystem itself does no
/// per-frame animation work; the managers (SceneSystems) do, driven by the scene.

module;
#include "Core/Prelude.h"

export module engine.animation:subsystem;

import foundation.core;
import engine.domain;
import foundation.runtime;         // Subsystem, Context
import foundation.scene; // Scene
import engine.scene; // SceneSubsystem (to register as scene-aware)
import :components;
import :propertyanimator;
import :attachment;
import :ik;

using namespace foundation::core;

export namespace engine::animation
{
    // The animation manager set for a scene. The subsystem injects it at runtime, and headless
    // scene consumers (Engine.Composition, for export and the MCP transcode scratch scene) call it
    // directly, so a manager added here reaches both.
    inline void AddAnimationSceneManagers(foundation::scene::Scene& scene)
    {
        scene.AddSystem<AnimationGraphComponentManager>();
        scene.AddSystem<SkeletalAnimationComponentManager>();
        scene.AddSystem<InstancedSkinningComponentManager>(); // crowd skinning (shared pose pool)
        scene.AddSystem<PropertyAnimatorComponentManager>();  // reflected property-curve animation
        AddIkSceneManagers(scene); // inverse kinematics: pose modifiers on the players above
        scene.AddSystem<BoneAttachmentComponentManager>(); // entities that follow a bone
    }

    // Registers the reflection, and draws the IK components that ask for it (debugDraw) into each
    // running scene's debug list, as navigation and physics do.
    class AnimationSubsystem final : public foundation::runtime::Subsystem,
                                     public foundation::scene::ISceneObserver
    {
    public:
        void OnSystemsReady(foundation::scene::Scene& scene) override { m_scenes.PushBack(&scene); }
        void OnDestroying(foundation::scene::Scene& scene) override
        {
            for (usize i = 0; i < m_scenes.Size(); ++i)
            {
                if (m_scenes[i] == &scene)
                {
                    m_scenes.RemoveAt(i);
                    return;
                }
            }
        }
        // Defined in the implementation unit (the render subsystem stays out of this interface).
        void Update(f32 deltaTime) override;

    protected:
        void OnInit() override
        {
            RegisterAnimationComponentReflection(); // tooling: reflected components (idempotent)
        }
        void OnReady() override
        {
            if (foundation::runtime::SubsystemManager* context = Manager())
            {
                if (auto* scenes = context->GetSubsystem<engine::scene::SceneSubsystem>())
                {
                    scenes->RegisterObserver(this, foundation::scene::SceneLifecycleStage::SystemsReady);
                    scenes->RegisterObserver(this, foundation::scene::SceneLifecycleStage::Destroying);
                }
            }
        }
        void OnShutdown() override
        {
            if (foundation::runtime::SubsystemManager* context = Manager())
            {
                if (auto* scenes = context->GetSubsystem<engine::scene::SceneSubsystem>())
                {
                    scenes->UnregisterObserver(this);
                }
            }
        }

    private:
        Array<foundation::scene::Scene*> m_scenes;
    };

} // namespace engine::animation

export namespace engine::animation
{
    /// This domain's declaration: what it brings to a scene, to reflection and to the script
    /// surface, and which resource modules come with it. Defined in the implementation unit (one
    /// instance per process); Engine.Composition lists it once.
    [[nodiscard]] const engine::DomainModule& AnimationDomain() noexcept;
}
