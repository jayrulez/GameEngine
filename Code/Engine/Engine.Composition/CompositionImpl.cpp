// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Engine::Composition - implementation unit (the wide domain imports live here, keeping the
// interface BMI lean). The full composition is built from the domains' own declarations:
// each Engine.<Domain> library defines its DomainModule beside the functions it names, so a
// manager, a registrar, a facade or a resource module added to a domain reaches every consumer.
// There is no parallel list to drift, only this one of the domains themselves.

module;
#include "Core/Prelude.h"

module engine.composition;

import foundation.core;
import foundation.scene;
import foundation.scene.resource; // PrefabSpawnSystem and the scene document types
import foundation.resource;
import foundation.script.facades; // the base behaviour facades (Entity/Log/Time/Random)
import engine.domain;
import engine.render;
import engine.animation;
import engine.particles;
import engine.physics;
import engine.terrain;
import engine.vegetation;
import engine.navigation;
import engine.audio;
import engine.script;
import engine.net;
import engine.spline;
import engine.input;
import engine.gameinstance;
import engine.gui; // the engine's GUI on foundation.gui

using namespace foundation::core;
namespace scene = foundation::scene;

namespace
{
    // The runtime prefab spawn (a script's scene.spawn, the replicated spawn) and the scene
    // document types: Foundation's, with no engine library of their own, so the root declares
    // them. Inert until the host points the system at its content (DefaultApplication, at
    // SystemsReady).
    const foundation::resource::ResourceModule* const kPrefabResources[] = {
        &scene::kSceneResourceModule};
    const engine::DomainModule kPrefabsDomain{
        .id = u8"prefabs",
        .installScene = &scene::AddPrefabSpawnSceneManagers,
        .resources = Span<const foundation::resource::ResourceModule* const>{kPrefabResources, 1}};

    // The one list. Order sets the default (dependency-free) instantiation order; the
    // facade-only domains follow the scene domains.
    const engine::DomainModule* const kAllDomains[] = {
        &engine::render::RenderDomain(),         &engine::animation::AnimationDomain(),
        &engine::particles::ParticleDomain(),    &engine::physics::PhysicsDomain(),
        &engine::terrain::TerrainDomain(),       &engine::vegetation::VegetationDomain(),
        &engine::navigation::NavigationDomain(), &engine::audio::AudioDomain(),
        &engine::script::ScriptDomain(),         &engine::net::NetDomain(),
        &engine::spline::SplineDomain(),         &kPrefabsDomain,
        &engine::input::InputDomain(),           &engine::runtime::RunDomain(),
        &engine::gui::GuiDomain(),
    };
}

namespace
{
    // The full composition and the plugin domains it is built from, beside the engine's list.
    struct CompositionState
    {
        struct PluginDomain
        {
            u64 token;
            const engine::DomainModule* module;
        };
        Array<PluginDomain> plugins;
        u64 nextToken = 1;
        engine::EngineComposition composition;

        void Rebuild()
        {
            Array<const engine::DomainModule*> modules;
            for (const engine::DomainModule* module : kAllDomains)
            {
                modules.PushBack(module);
            }
            for (const PluginDomain& plugin : plugins)
            {
                modules.PushBack(plugin.module);
            }
            composition = engine::EngineComposition::Build(
                Span<const engine::DomainModule* const>{modules.Data(), modules.Size()});
        }
    };

    CompositionState& State()
    {
        static CompositionState state;
        static const bool built = (state.Rebuild(), true); // the first build, thread-safe
        (void)built;
        return state;
    }
}

namespace engine
{
    const EngineComposition& FullComposition() { return State().composition; }

    u64 AddPluginDomain(const DomainModule& module)
    {
        if (module.installScene != nullptr)
        {
            return 0;
        }
        CompositionState& state = State();
        const u64 token = state.nextToken++;
        state.plugins.PushBack(CompositionState::PluginDomain{token, &module});
        state.Rebuild();
        // What the engine registers at startup for its own domains, this one registers now: a
        // host may have done its startup registration before the plugin loaded. Each is
        // idempotent, and a plugin host records what they register, so unload reverses it.
        if (module.registerReflection != nullptr)
        {
            module.registerReflection();
        }
        if (module.registerScriptFacade != nullptr)
        {
            module.registerScriptFacade();
        }
        for (const foundation::resource::ResourceModule* resources : module.resources)
        {
            resources->RegisterTypes();
        }
        return token;
    }

    void RemovePluginDomain(u64 token)
    {
        CompositionState& state = State();
        for (usize i = 0; i < state.plugins.Size(); ++i)
        {
            if (state.plugins[i].token == token)
            {
                state.plugins.RemoveAt(i);
                state.Rebuild();
                return;
            }
        }
    }

    const scene::SceneComposition& FullSceneComposition() { return FullComposition().Scene(); }

    void AddAllSceneManagers(scene::Scene& scene) { FullComposition().Scene().Instantiate(scene); }

    void RegisterAllSceneComponentReflection()
    {
        scene::RegisterEntityRefReflection(); // what components' entity references are, by their id
        FullComposition().RegisterReflection();
    }

    void RegisterAllResourceTypes() { FullComposition().RegisterResourceTypes(); }

    void RegisterAllScriptFacades()
    {
        // Core value types and the base behaviour facades first (idempotent), then every domain's.
        RegisterCoreTypes();
        foundation::script::RegisterScriptFacadeReflection();
        FullComposition().RegisterScriptFacades();
    }
}
