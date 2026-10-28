// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Engine::Composition - implementation unit (the wide domain imports live here, keeping the
// interface BMI lean). The full composition is built ONCE from the domains' own declarations:
// each Engine.<Domain> library defines its DomainModule beside the functions it names, so a
// manager, a registrar, a facade or a resource module added to a domain reaches every consumer
// automatically - there is no parallel list to drift, only this one of the domains themselves.

module;
#include "Core/Prelude.h"

module engine.composition;

import foundation.core;
import foundation.scene;
import foundation.scene.resource; // PrefabSpawnSystem + the scene document types
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
import engine.gui; // the engine's GUI on foundation.gui, beside the UI domain while both live

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

    // The one list. Order binds the default (dependency-free) instantiation order and mirrors
    // the old scene list byte for byte; the facade-only domains follow.
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

namespace engine
{
    const EngineComposition& FullComposition()
    {
        static const EngineComposition composition =
            EngineComposition::Build(Span<const DomainModule* const>{kAllDomains});
        return composition;
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
        // Core value types + the base behaviour facades first (idempotent), then every domain's.
        RegisterCoreTypes();
        foundation::script::RegisterScriptFacadeReflection();
        FullComposition().RegisterScriptFacades();
    }
}
