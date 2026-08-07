// Draconic.Particles - effect-graph reflection (out of the interface: GCC module hygiene keeps
// DRACONIC_REFLECT bodies in an impl unit). Reflects the value types that make up an authored effect
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
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

module draconic.particles;

import draconic.foundation;

using namespace draconic::foundation;

namespace draconic::particles
{
    DRACONIC_REFLECT_ENUM(EmissionMode, "draconic::particles")
    {
        builder.Value("Continuous", EmissionMode::Continuous);
        builder.Value("Burst", EmissionMode::Burst);
        builder.Value("ContinuousAndBurst", EmissionMode::ContinuousAndBurst);
    }

    DRACONIC_REFLECT_VALUE(ParticleEmitter, "draconic::particles")
    {
        builder.Property<&ParticleEmitter::mode>("mode")
            .Property<&ParticleEmitter::spawnRate>("spawnRate")
            .Property<&ParticleEmitter::burstCount>("burstCount")
            .Property<&ParticleEmitter::burstInterval>("burstInterval")
            .Property<&ParticleEmitter::burstCycles>("burstCycles")
            .Property<&ParticleEmitter::isEmitting>("isEmitting")
            .Property<&ParticleEmitter::duration>("duration")
            .Property<&ParticleEmitter::looping>("looping");
    }

    // Member hook: has private access to m_initializers / m_behaviors.
    void ParticleSystem::BuildReflection(TypeBuilder<ParticleSystem>& builder)
    {
        builder.Property<&ParticleSystem::name>("name")
            .Property<&ParticleSystem::desiredMode>("desiredMode")
            .Property<&ParticleSystem::simulationSpace>("simulationSpace")
            .Property<&ParticleSystem::blendMode>("blendMode")
            .Property<&ParticleSystem::renderMode>("renderMode")
            .Property<&ParticleSystem::sortParticles>("sortParticles")
            .Property<&ParticleSystem::softParticles>("softParticles")
            .Property<&ParticleSystem::softDistance>("softDistance")
            .Property<&ParticleSystem::prewarmTime>("prewarmTime")
            .Property<&ParticleSystem::lodStartDistance>("lodStartDistance")
            .Property<&ParticleSystem::lodCullDistance>("lodCullDistance")
            .Property<&ParticleSystem::lodMinRate>("lodMinRate")
            .Nested<&ParticleSystem::emitter>("emitter")
            .Nested<&ParticleSystem::m_initializers>("initializers") // polymorphic container
            .Nested<&ParticleSystem::m_behaviors>("behaviors");      // polymorphic container
    }
    DRACONIC_REFLECT_VALUE(ParticleSystem, "draconic::particles")
    {
        ParticleSystem::BuildReflection(builder);
    }

    // Member hook: has private access to m_systems.
    void ParticleEffect::BuildReflection(TypeBuilder<ParticleEffect>& builder)
    {
        builder.Property<&ParticleEffect::name>("name")
            .Nested<&ParticleEffect::m_systems>("systems"); // Array<UniquePtr<ParticleSystem>>
    }
    DRACONIC_REFLECT_VALUE(ParticleEffect, "draconic::particles")
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
        DraconicRegisterEnum_EmissionMode();
        DraconicRegisterValue_ParticleEmitter();
        DraconicRegisterValue_ParticleSystem();
        DraconicRegisterValue_ParticleEffect();
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
