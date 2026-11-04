// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// nativesample.engine - the reference game's runtime types: a component the game puts on an entity,
// and the scene manager that owns it, which the Engine part adds to every scene.

module;
#include "Core/Prelude.h"

export module nativesample.engine;

import foundation.core;
import foundation.scene;

export namespace nativesample
{
    // How fast an entity spins, in turns a second.
    struct SpinComponent
    {
        foundation::core::f32 turnsPerSecond = 0.25f;
    };

    inline void Serialize(foundation::core::ISerializer& ar, SpinComponent& component)
    {
        foundation::core::Serialize(ar, "turnsPerSecond", component.turnsPerSecond);
    }

    // The spin components of a scene, saved with it.
    class SpinManager final : public foundation::scene::SerializableComponentManager<SpinComponent>
    {
    public:
        SpinManager() : SerializableComponentManager<SpinComponent>(u8"nativesample.Spin") {}
    };

    // The id the Engine part contributes the manager under.
    inline constexpr foundation::core::StringView kSpinContribution = u8"nativesample.spin";
}
