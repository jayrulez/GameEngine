// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// NativeSample.Engine's entry, the reference game's runtime code. Load is the one registration point:
// it adds the spin manager to every scene, live ones included, through the engine's service (a host
// without one, such as the cook, offers none, and nothing is added). What it adds is recorded and
// taken out again when the plugin unloads (a reload, the project closing).

#include "Core/Prelude.h"
#include "Core/Log/Log.h"
#include "Runtime/PluginEntry.h"

import foundation.core;
import foundation.runtime;
import foundation.scene;
import engine.composition; // EnginePluginServices
import nativesample.engine;

namespace
{
    void Load(foundation::runtime::PluginContext& context)
    {
        engine::EnginePluginServices* engine = context.Find<engine::EnginePluginServices>();
        if (engine == nullptr)
        {
            return;
        }
        foundation::scene::SceneModuleContributions::Contribution spin;
        spin.id = nativesample::kSpinContribution;
        spin.install = [](foundation::scene::Scene& scene) { scene.AddSystem<nativesample::SpinManager>(); };
        spin.systemType = foundation::core::TypeOf<nativesample::SpinManager>().id;
        engine->AddSceneManager(spin);
        LOG_INFO(u8"Game", u8"NativeSample loaded");
    }
}

PLUGIN_PART(&Load, nullptr)
