// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Particles.Pipeline - the particles domain's New Asset creators: what File > New and
// asset_create make, with no editor (the source database and the sources folder are all a
// creation needs).

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

module particles.pipeline;

import foundation.core;
import foundation.content;
import foundation.particles;
import pipeline.core;

using namespace foundation::core;

namespace pipeline
{

    void SeedDefaultParticleEffect(foundation::particles::ParticleEffect& fx)
    {
        (void)AddDefaultParticleSystem(fx);
    }

    foundation::particles::ParticleSystem& AddDefaultParticleSystem(foundation::particles::ParticleEffect& fx)
    {
        namespace particles = foundation::particles;
        particles::ParticleSystem& sys = fx.AddSystem(2000);
        sys.AddInitializer<particles::LifetimeInitializer>().lifetime =
            particles::RangeFloat(1.5f, 2.5f);
        sys.AddInitializer<particles::VelocityInitializer>().baseVelocity =
            Float3{0.0f, 5.0f, 0.0f};
        sys.AddInitializer<particles::SizeInitializer>();
        sys.AddInitializer<particles::ColorInitializer>();
        sys.AddBehavior<particles::GravityBehavior>();
        sys.emitter.mode = particles::EmissionMode::Continuous;
        sys.emitter.spawnRate = 120.0f;
        return sys;
    }

    void RegisterParticleCreators(AssetCreatorRegistry& registry)
    {
        {
            // One continuous fountain system, so a new effect shows something at once.
            AssetCreator creator;
            creator.label = String(u8"Particle Effect");
            creator.type = &ParticleEffectAsset::StaticType();
            creator.defaultGroup = String(u8"ParticleEffects");
            creator.run = [](const AssetCreationContext& context) -> foundation::content::Instance*
            {
                ParticleEffectAsset asset;
                SeedDefaultParticleEffect(asset.Effect());
                return CreateWrittenInstance(context.Target(), context.NameOr(u8"ParticleEffect"),
                                             ParticleEffectAsset::StaticType(), asset);
            };
            registry.Register(Move(creator));
        }
    }

    // The asset types' reflection: their display names (the editor shows them as the asset's kind).
    REFLECT_MEMBERS(ParticleEffectAsset, "rtti::pipeline::particles")
    {
        // v1: each system its sub-emitters; v0's effect-level links read onto them.
        builder.DataVersion(1).ReadsDataVersionsFrom(0).Attribute("displayName", String(u8"Particle Effect"));
    }
}
