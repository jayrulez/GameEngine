// Draconic Particles - particles.subsystem implementation unit: the component reflection body.
//
// Kept OUT of the :components interface partition: DRACONIC_REFLECT_* bodies in an interface
// unit make GCC emit an unreadable gcm cluster for consumers (see gcc-module-interface-hygiene).
// ParticleComponents.cppm declares RegisterParticleComponentReflection(); this unit defines it.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

module draconic.engine.particles;

import draconic.foundation;
import draconic.script.facades; // ComponentOf<T> + RegisterExtra* (the script `.of` surface, Track A)

using namespace draconic::foundation;

namespace draconic::particles
{
    DRACONIC_REFLECT_VALUE(ParticleEffectComponent, "draconic::particles")
    {
        builder.Attribute("displayName", String(u8"Particle Effect"))
            .Attribute("category", String(u8"Effects"))
            // Script (Track A): ParticleEffectComponent.of(entity) -> live visible/meshScale/light*.
            // play/stop/setEffect are instance ops -> SceneParticles.of(scene) (world ops keyed by entity).
            .Method<&draconic::script::ComponentOf<ParticleEffectComponent>, ParticleEffectComponent>(
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
    DRACONIC_REFLECT_VALUE(SceneParticles, "draconic::particles")
    {
        builder.Method<&SceneParticles::play>("play", {"entity"});
        builder.Method<&SceneParticles::stop>("stop", {"entity"});
        builder.Method<&SceneParticles::restart>("restart", {"entity"});
        builder.Method<&SceneParticles::pause>("pause", {"entity", "paused"});
        builder.Method<&SceneParticles::isPlaying>("isPlaying", {"entity"});
        builder.Method<&SceneParticles::setEffect>("setEffect", {"entity", "resourceId"});
        builder.Method<&SceneParticles::of>("of", {"scene"});
        builder.Constructor(); // Wren only materializes constructible foreign classes
    }

    void RegisterParticleComponentReflection()
    {
        static const bool once = []()
        {
            DraconicRegisterValue_ParticleEffectComponent();
            return true;
        }();
        (void)once;
    }

    void RegisterParticleScriptFacade()
    {
        RegisterParticleComponentReflection(); // ensure component TypeData (incl `of`) is built first
        // Surface the particle component (ParticleEffectComponent.of(entity)): register + seed root + name.
        GlobalTypeRegistry().Register(foundation::TypeOf<ParticleEffectComponent>());
        draconic::script::RegisterExtraScriptRootType(&foundation::TypeOf<ParticleEffectComponent>());
        draconic::script::RegisterExtraFacadeName(u8"ParticleEffectComponent");

        // The scene-bound particles handle (SceneParticles.of(scene)): reflect + register + seed + name.
        DraconicRegisterValue_SceneParticles();
        GlobalTypeRegistry().Register(foundation::TypeOf<SceneParticles>());
        draconic::script::RegisterExtraScriptRootType(&foundation::TypeOf<SceneParticles>());
        draconic::script::RegisterExtraFacadeName(u8"SceneParticles");
    }
}
