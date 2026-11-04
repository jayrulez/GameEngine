// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// nativesample.engine - the reference game's runtime types: a component the game puts on an entity,
// and the scene manager that owns it and turns its entities, which the Engine part adds to every
// scene.

module;
#include "Core/Prelude.h"

export module nativesample.engine;

import foundation.core;
import foundation.scene;

export namespace nativesample
{
    // Turns its entity about an axis of the entity's own (its local up by default) while the scene
    // simulates.
    struct SpinComponent
    {
        foundation::core::f32 turnsPerSecond = 0.25f;
        foundation::core::Float3 axis{0.0f, 1.0f, 0.0f};
    };

    inline void Serialize(foundation::core::ISerializer& ar, SpinComponent& component)
    {
        foundation::core::Serialize(ar, "turnsPerSecond", component.turnsPerSecond);
        foundation::core::Serialize(ar, "axis", component.axis);
    }

    // The spin components of a scene, saved with it; each update turns their entities.
    class SpinManager final : public foundation::scene::SerializableComponentManager<SpinComponent>
    {
    public:
        SpinManager() : SerializableComponentManager<SpinComponent>(u8"nativesample.Spin") {}

        void OnSceneCreate(foundation::scene::Scene& scene) override { m_scene = &scene; }

        // The editor's edit mode does not turn them; a simulating page, play and the player do.
        [[nodiscard]] bool IsSimulationOnly() const noexcept override { return true; }

        void OnUpdate(foundation::scene::ScenePhase phase, foundation::core::f32 deltaTime) override
        {
            if (phase != foundation::scene::ScenePhase::Update || m_scene == nullptr)
            {
                return;
            }
            ForEach(
                [&](SpinComponent& spin, foundation::scene::EntityHandle owner)
                {
                    if (!m_scene->IsEffectivelyActive(owner))
                    {
                        return;
                    }
                    const foundation::core::Quaternion turn = foundation::core::Quaternion::FromAxisAngle(
                        spin.axis, spin.turnsPerSecond * foundation::core::kTwoPi * deltaTime);
                    // The turn applies in the entity's own frame; normalizing keeps the rotation a
                    // unit one over many frames of accumulated products.
                    foundation::core::Transform local = m_scene->GetLocalTransform(owner);
                    local.rotation = foundation::core::Normalized(local.rotation * turn);
                    m_scene->SetLocalTransform(owner, local);
                });
        }

    private:
        foundation::scene::Scene* m_scene = nullptr;
    };

    // The id the Engine part contributes the manager under.
    inline constexpr foundation::core::StringView kSpinContribution = u8"nativesample.spin";
}
