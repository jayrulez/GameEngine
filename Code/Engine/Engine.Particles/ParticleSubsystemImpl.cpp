// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Particles - particles.subsystem implementation unit: the component reflection body.
//
// Kept OUT of the :components interface partition: REFLECT_* bodies in an interface
// unit make GCC emit an unreadable gcm cluster for consumers (see gcc-module-interface-hygiene).
// ParticleComponents.cppm declares RegisterParticleComponentReflection(); this unit defines it.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

module engine.particles;
import engine.domain;
import foundation.particles.resource;

import foundation.core;
import foundation.script.facades; // ComponentOf<T> + RegisterExtra* (the script `.of` surface, Track A)

using namespace foundation::core;
namespace core = foundation::core;

namespace engine::particles
{
    REFLECT_VALUE(ParticleEffectComponent, "rtti::engine::particles")
    {
        builder.Attribute("displayName", String(u8"Particle Effect"))
            .Attribute("category", String(u8"Effects"))
            .Attribute("description",
                       String(u8"Plays a particle effect at the entity: billboards, trails, meshes "
                              u8"or lights."))
            // Script (Track A): ParticleEffectComponent.of(entity) -> live visible/meshScale/light*.
            // play/stop/setEffect are instance ops -> SceneParticles.of(scene) (world ops keyed by entity).
            .Method<&foundation::script::ComponentOf<ParticleEffectComponent>, ParticleEffectComponent>(
                "of")
            .Property<&ParticleEffectComponent::effectAsset>("effect")
            .Property<&ParticleEffectComponent::mesh>("mesh")
            .Property<&ParticleEffectComponent::material>("material")
            .Property<&ParticleEffectComponent::meshScale>("meshScale")
            .Property<&ParticleEffectComponent::lightIntensity>("lightIntensity")
            .Property<&ParticleEffectComponent::lightRange>("lightRange")
            .Property<&ParticleEffectComponent::visible>("visible");
    }

    // The scene-bound particles handle: SceneParticles.of(scene).play/stop/restart/pause/isPlaying/
    // setEffect. `of` returns SceneParticles by value (concrete cross-backend return), like ScenePhysics.
    REFLECT_VALUE(SceneParticles, "rtti::engine::particles")
    {
        builder.Method<&SceneParticles::play>("play", {"entity"});
        builder.Method<&SceneParticles::stop>("stop", {"entity"});
        builder.Method<&SceneParticles::restart>("restart", {"entity"});
        builder.Method<&SceneParticles::pause>("pause", {"entity", "paused"});
        builder.Method<&SceneParticles::isPlaying>("isPlaying", {"entity"});
        builder.Method<&SceneParticles::setEffect>("setEffect", {"entity", "resourceId"});
        builder.Method<&SceneParticles::of>("of", {"scene"});
        builder.Constructor(); // some backends only materialize constructible foreign classes
    }

    void RegisterParticleComponentReflection()
    {
        static const bool once = []()
        {
            RttiRegisterValue_ParticleEffectComponent();
            return true;
        }();
        (void)once;
    }

    void RegisterParticleScriptFacade()
    {
        RegisterParticleComponentReflection(); // ensure component TypeData (incl `of`) is built first
        // Surface the particle component (ParticleEffectComponent.of(entity)): register + seed root + name.
        GlobalTypeRegistry().Register(core::TypeOf<ParticleEffectComponent>());
        foundation::script::RegisterExtraScriptRootType(&core::TypeOf<ParticleEffectComponent>());
        foundation::script::RegisterExtraFacadeName(u8"ParticleEffectComponent");

        // The scene-bound particles handle (SceneParticles.of(scene)): reflect + register + seed + name.
        RttiRegisterValue_SceneParticles();
        GlobalTypeRegistry().Register(core::TypeOf<SceneParticles>());
        foundation::script::RegisterExtraScriptRootType(&core::TypeOf<SceneParticles>());
        foundation::script::RegisterExtraFacadeName(u8"SceneParticles");
    }
}

namespace engine::particles
{
    const engine::DomainModule& ParticleDomain() noexcept
    {
        static const foundation::resource::ResourceModule* const kResources[] = {
            &foundation::particles::kParticlesResourceModule};
        static const engine::DomainModule kModule{
            .id = u8"particles",
            .installScene = &AddParticleSceneManagers,
            .registerReflection = &RegisterParticleComponentReflection,
            .registerScriptFacade = &RegisterParticleScriptFacade,
            .resources = foundation::core::Span<const foundation::resource::ResourceModule* const>{
                kResources, sizeof(kResources) / sizeof(kResources[0])}};
        return kModule;
    }
}
