// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// engine.scene - implementation unit: the scene-manager contribution recorder.
// A PluginHost arms it around a plugin's OnLoad so the managers the plugin contributes
// are recorded and reversed on unload, leaving live scenes while the code that built
// them is still mapped.

module;
#include "Core/Prelude.h"

module engine.scene;

using namespace foundation::core;
using namespace foundation::scene;

namespace engine::scene
{
    void SceneContributionRecorder::Arm(Array<TypeId>& sink)
    {
        SceneModuleContributions::Global().SetRegistrationObserver(
            [](void* ctx, TypeId id) { static_cast<Array<TypeId>*>(ctx)->PushBack(id); }, &sink);
    }

    void SceneContributionRecorder::Disarm()
    {
        SceneModuleContributions::Global().SetRegistrationObserver(nullptr, nullptr);
    }

    void SceneContributionRecorder::Reverse(TypeId id)
    {
        SceneModuleContributions::Global().Remove(id);
    }

    SceneContributionRecorder& GlobalSceneContributionRecorder() noexcept
    {
        static SceneContributionRecorder instance; // one per process (rendezvous rule)
        return instance;
    }
} // namespace engine::scene
