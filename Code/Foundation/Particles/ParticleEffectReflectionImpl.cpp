// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Particles - effect-graph reflection (out of the interface: GCC module hygiene keeps
// REFLECT_MEMBERS bodies in an impl unit). Reflects the value types that make up an authored effect
// so tooling and scripting can traverse it end to end:
//
//     ParticleEffectAsset --Nested--> ParticleEffect
//         .name
//         .systems  (Array<UniquePtr<ParticleSystem>>, homogeneous UniquePtr container)
//             --> ParticleSystem
//                 .emitter        (Nested ParticleEmitter)
//                 .initializers   (Array<RefPtr<ParticleInitializer>>, polymorphic container)
//                 .behaviors      (Array<RefPtr<ParticleBehavior>>,    polymorphic container)
//                     --> each concrete module's reflected props --> ranges / curves
//
// The two module arrays are private on ParticleSystem and the systems array is private on
// ParticleEffect; each type's static BuildReflection() member (declared in the interface, defined
// here) reaches them without widening the public surface. The polymorphic module containers are
// registered by RegisterParticleModuleReflection(); this unit adds the UniquePtr systems container.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

module foundation.particles;

import foundation.core;

using namespace foundation::core;

namespace foundation::particles
{
    // A system's modes, so a row names them (they were reflected as bare values, no row could).
    REFLECT_ENUM(SimulationMode, "rtti::particles")
    {
        builder.Value("CPU", SimulationMode::CPU);
        builder.Value("GPU", SimulationMode::GPU);
        builder.Value("Auto", SimulationMode::Auto);
    }
    REFLECT_ENUM(ParticleSpace, "rtti::particles")
    {
        builder.Value("World", ParticleSpace::World);
        builder.Value("Local", ParticleSpace::Local);
    }
    REFLECT_ENUM(ParticleBlendMode, "rtti::particles")
    {
        builder.Value("Alpha", ParticleBlendMode::Alpha);
        builder.Value("Additive", ParticleBlendMode::Additive);
        builder.Value("Premultiplied", ParticleBlendMode::Premultiplied);
        builder.Value("Multiply", ParticleBlendMode::Multiply);
    }
    REFLECT_ENUM(ParticleRenderMode, "rtti::particles")
    {
        builder.Value("Billboard", ParticleRenderMode::Billboard);
        builder.Value("StretchedBillboard", ParticleRenderMode::StretchedBillboard);
        builder.Value("HorizontalBillboard", ParticleRenderMode::HorizontalBillboard);
        builder.Value("VerticalBillboard", ParticleRenderMode::VerticalBillboard);
        builder.Value("Mesh", ParticleRenderMode::Mesh);
        builder.Value("Trail", ParticleRenderMode::Trail);
        builder.Value("Light", ParticleRenderMode::Light);
    }

    REFLECT_ENUM(ParticleEventType, "rtti::particles")
    {
        builder.Value("OnBirth", ParticleEventType::OnBirth);
        builder.Value("OnDeath", ParticleEventType::OnDeath);
    }

    // A system's sub-emitter: on its particles' births or deaths, spawns into the system it names.
    REFLECT_VALUE(SubEmitterLink, "rtti::particles")
    {
        builder.Attribute("displayName", String(u8"Sub-emitter"));
        builder.Property<&SubEmitterLink::trigger>("trigger")
            .PropAttribute("description", String(u8"When this system's particles spawn it: as they are born, or as they die"))
            .Property<&SubEmitterLink::childSystemIndex>("childSystemIndex")
            .PropAttribute("displayName", String(u8"Into"))
            .PropAttribute("description", String(u8"The system it spawns into (with Emits off, that system shows only what is spawned)"))
            .Property<&SubEmitterLink::spawnCount>("spawnCount")
            .PropAttribute("range", Float4{1.0f, 1000.0f, 1.0f, 0.0f})
            .Property<&SubEmitterLink::probability>("probability")
            .PropAttribute("description", String(u8"The chance it spawns at all, for each particle"))
            .PropAttribute("range", Float4{0.0f, 1.0f, 0.01f, 0.0f})
            .Property<&SubEmitterLink::inheritPosition>("inheritPosition")
            .PropAttribute("description", String(u8"Where the particle was; off, where the child system emits"))
            .Property<&SubEmitterLink::inheritVelocity>("inheritVelocity")
            .PropAttribute("visibleWhen", String(u8"inheritPosition"))
            .Property<&SubEmitterLink::velocityInheritFactor>("velocityInheritFactor")
            .PropAttribute("displayName", String(u8"Of Its Velocity"))
            .PropAttribute("range", Float4{0.0f, 1.0f, 0.01f, 0.0f})
            .PropAttribute("visibleWhen", String(u8"inheritVelocity"))
            .Property<&SubEmitterLink::inheritColor>("inheritColor")
            .PropAttribute("visibleWhen", String(u8"inheritPosition"));
    }

    REFLECT_ENUM(EmissionMode, "rtti::particles")
    {
        builder.Value("Continuous", EmissionMode::Continuous);
        builder.Value("Burst", EmissionMode::Burst);
        builder.Value("ContinuousAndBurst", EmissionMode::ContinuousAndBurst);
    }

    // Each row where it does something: the spawn rate in the continuous modes, the bursts in the
    // burst modes (`visibleWhen` of the mode's raw value).
    REFLECT_VALUE(ParticleEmitter, "rtti::particles")
    {
        builder.Property<&ParticleEmitter::mode>("mode")
            .Property<&ParticleEmitter::spawnRate>("spawnRate")
            .PropAttribute("visibleWhen", String(u8"mode=0,2"))
            .PropAttribute("description", String(u8"Particles a second"))
            .Property<&ParticleEmitter::burstCount>("burstCount")
            .PropAttribute("visibleWhen", String(u8"mode=1,2"))
            .Property<&ParticleEmitter::burstInterval>("burstInterval")
            .PropAttribute("visibleWhen", String(u8"mode=1,2"))
            .PropAttribute("description", String(u8"Seconds between bursts; 0 bursts once"))
            .Property<&ParticleEmitter::burstCycles>("burstCycles")
            .PropAttribute("visibleWhen", String(u8"mode=1,2"))
            .PropAttribute("description", String(u8"Bursts in all; 0 bursts on and on"))
            .Property<&ParticleEmitter::isEmitting>("isEmitting")
            .PropAttribute("displayName", String(u8"Emits"))
            .PropAttribute("description", String(u8"Off: it emits only when another system's sub-emitter link spawns it"))
            .Property<&ParticleEmitter::duration>("duration")
            .PropAttribute("description", String(u8"Seconds it emits; 0 emits on and on"))
            .Property<&ParticleEmitter::looping>("looping")
            .PropAttribute("description", String(u8"With a duration: emits for it again and again"));
    }

    REFLECT_VALUE(TrailSettings, "rtti::particles")
    {
        builder.Property<&TrailSettings::enabled>("enabled")
            .Property<&TrailSettings::maxPoints>("maxPoints")
            .PropAttribute("range", Float4{2.0f, 256.0f, 1.0f, 0.0f})
            .Property<&TrailSettings::recordInterval>("recordInterval")
            .PropAttribute("description", String(u8"Seconds at least between the points it records"))
            .Property<&TrailSettings::lifetime>("lifetime")
            .PropAttribute("description", String(u8"Seconds a point takes to fade"))
            .Property<&TrailSettings::widthStart>("widthStart")
            .Property<&TrailSettings::widthEnd>("widthEnd")
            .Property<&TrailSettings::minVertexDistance>("minVertexDistance")
            .PropAttribute("displayName", String(u8"Min Point Distance"))
            .Property<&TrailSettings::useParticleColor>("useParticleColor")
            .Property<&TrailSettings::trailColor>("trailColor")
            .PropAttribute("color", true)
            .PropAttribute("visibleWhen", String(u8"useParticleColor=0"));
    }

    REFLECT_VALUE(FlipbookSettings, "rtti::particles")
    {
        builder.Property<&FlipbookSettings::enabled>("enabled")
            .Property<&FlipbookSettings::columns>("columns")
            .PropAttribute("range", Float4{1.0f, 64.0f, 1.0f, 0.0f})
            .Property<&FlipbookSettings::rows>("rows")
            .PropAttribute("range", Float4{1.0f, 64.0f, 1.0f, 0.0f})
            .Property<&FlipbookSettings::overLifetime>("overLifetime")
            .PropAttribute("description", String(u8"The whole sheet once over a particle's life; off, at its frame rate"))
            .Property<&FlipbookSettings::fps>("fps")
            .PropAttribute("displayName", String(u8"Frame Rate"))
            .PropAttribute("visibleWhen", String(u8"overLifetime=0"))
            .Property<&FlipbookSettings::startFrame>("startFrame");
    }

    // Member hook: has private access to m_initializers / m_behaviors.
    // Every authored field, each row where it does something (`visibleWhen` of the render mode's
    // raw value: 0 to 3 the billboards, 4 a mesh, 5 a trail, 6 a light): a texture for the
    // billboards and the trail, a flipbook for the billboards, a mesh and its materials for a mesh.
    // The references are bare ids naming what they bind (the resource factory binds them).
    void ParticleSystem::BuildReflection(TypeBuilder<ParticleSystem>& builder)
    {
        builder.Property<&ParticleSystem::name>("name")
            .Property<&ParticleSystem::desiredMode>("desiredMode")
            .PropAttribute("displayName", String(u8"Simulation"))
            .Property<&ParticleSystem::simulationSpace>("simulationSpace")
            .PropAttribute("displayName", String(u8"Space"))
            .Property<&ParticleSystem::blendMode>("blendMode")
            .Property<&ParticleSystem::renderMode>("renderMode")
            .AccessorProperty<&ParticleSystem::MaxParticles, &ParticleSystem::SetMaxParticles>("maxParticles")
            .PropAttribute("range", Float4{1.0f, 1000000.0f, 1.0f, 0.0f})
            .PropAttribute("description", String(u8"The most alive at once"))
            .Property<&ParticleSystem::textureRef>("textureRef")
            .PropAttribute("displayName", String(u8"Texture"))
            .PropAttribute("referenceType", String(u8"Texture"))
            .PropAttribute("visibleWhen", String(u8"renderMode=0,1,2,3,5"))
            .Property<&ParticleSystem::meshRef>("meshRef")
            .PropAttribute("displayName", String(u8"Mesh"))
            .PropAttribute("referenceType", String(u8"StaticMesh"))
            .PropAttribute("visibleWhen", String(u8"renderMode=4"))
            .Property<&ParticleSystem::meshScale>("meshScale")
            .PropAttribute("visibleWhen", String(u8"renderMode=4"))
            .Nested<&ParticleSystem::materialRefs>("materialRefs")
            .PropAttribute("displayName", String(u8"Materials"))
            .PropAttribute("referenceType", String(u8"Material"))
            .PropAttribute("description", String(u8"By the mesh's material slots; the first the whole mesh's"))
            .PropAttribute("visibleWhen", String(u8"renderMode=4"))
            .Property<&ParticleSystem::sortParticles>("sortParticles")
            .Property<&ParticleSystem::softParticles>("softParticles")
            .Property<&ParticleSystem::softDistance>("softDistance")
            .PropAttribute("range", Float4{0.0f, 10.0f, 0.01f, 0.0f})
            .PropAttribute("visibleWhen", String(u8"softParticles"))
            .Property<&ParticleSystem::prewarmTime>("prewarmTime")
            .PropAttribute("description", String(u8"Seconds run before it shows, so it starts full"))
            .Property<&ParticleSystem::lodStartDistance>("lodStartDistance")
            .PropAttribute("displayName", String(u8"Fewer From"))
            .PropAttribute("description", String(u8"Its rate falls from this distance; 0 never"))
            .Property<&ParticleSystem::lodCullDistance>("lodCullDistance")
            .PropAttribute("displayName", String(u8"Hidden From"))
            .PropAttribute("description", String(u8"Not drawn past this distance; 0 always drawn"))
            .Property<&ParticleSystem::lodMinRate>("lodMinRate")
            .PropAttribute("displayName", String(u8"Fewest"))
            .PropAttribute("description", String(u8"Its rate at its farthest, as a fraction"))
            .PropAttribute("range", Float4{0.0f, 1.0f, 0.01f, 0.0f})
            .Nested<&ParticleSystem::flipbook>("flipbook")
            .PropAttribute("visibleWhen", String(u8"renderMode=0,1,2,3"))
            .Nested<&ParticleSystem::trail>("trail")
            .PropAttribute("visibleWhen", String(u8"renderMode=5"))
            .Nested<&ParticleSystem::emitter>("emitter")
            .Nested<&ParticleSystem::m_initializers>("initializers") // polymorphic container
            .Nested<&ParticleSystem::m_behaviors>("behaviors")       // polymorphic container
            .Nested<&ParticleSystem::subEmitters>("subEmitters")
            .PropAttribute("displayName", String(u8"Sub-emitters"));
    }
    REFLECT_VALUE(ParticleSystem, "rtti::particles")
    {
        ParticleSystem::BuildReflection(builder);
    }

    // Member hook: has private access to m_systems.
    void ParticleEffect::BuildReflection(TypeBuilder<ParticleEffect>& builder)
    {
        builder.Property<&ParticleEffect::name>("name")
            .Nested<&ParticleEffect::m_systems>("systems"); // Array<UniquePtr<ParticleSystem>>
    }
    REFLECT_VALUE(ParticleEffect, "rtti::particles")
    {
        ParticleEffect::BuildReflection(builder);
    }

    void RegisterParticleEffectReflection()
    {
        static bool registered = false;
        if (registered)
        {
            return;
        }
        registered = true;
        RttiRegisterEnum_SimulationMode();
        RttiRegisterEnum_ParticleSpace();
        RttiRegisterEnum_ParticleBlendMode();
        RttiRegisterEnum_ParticleRenderMode();
        RttiRegisterEnum_EmissionMode();
        RttiRegisterValue_ParticleEmitter();
        RttiRegisterValue_TrailSettings();
        RttiRegisterValue_FlipbookSettings();
        RttiRegisterEnum_ParticleEventType();
        RttiRegisterValue_SubEmitterLink();
        RegisterArrayType<SubEmitterLink>(); // a system's sub-emitters
        RegisterArrayType<Guid>(); // a mesh system's materials
        RttiRegisterValue_ParticleSystem();
        RttiRegisterValue_ParticleEffect();
        // The systems array is a homogeneous UniquePtr container; the module arrays are polymorphic
        // containers already registered by RegisterParticleModuleReflection().
        RegisterUniquePtrArrayType<ParticleSystem>();
        // Publish the effect-graph value types to the global type registry so the script harvest sees
        // them: a facade returning a ParticleEffect / ParticleSystem handle makes the reachability
        // closure emit the whole graph (systems -> modules -> ranges/curves) as scriptable surface.
        // Inert until such a facade exists (no constructor -> never a script-emit seed on its own).
        GlobalTypeRegistry().Register(TypeOf<ParticleEmitter>());
        GlobalTypeRegistry().Register(TypeOf<ParticleSystem>());
        GlobalTypeRegistry().Register(TypeOf<ParticleEffect>());
    }
}
