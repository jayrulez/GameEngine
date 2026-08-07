// ParticleEffectEditorPage tests (headless): the "New Particle Effect" seed is a free, pure
// function so it is covered here without a live host/renderer. The page's GPU preview + property
// grid + component wiring need a live application host and are exercised in the editor app.

#include <doctest/doctest.h>

#include "Draconic.Foundation/Prelude.h"

import draconic.foundation;
import draconic.particles;
import draconic.editor.scene;

using namespace draconic::foundation;
namespace particles = draconic::particles;

TEST_CASE("particle page: default seed builds a one-system fountain with the core modules")
{
    particles::ParticleEffect fx;
    draconic::editor::SeedDefaultParticleEffect(fx);

    REQUIRE(fx.SystemCount() == 1);
    particles::ParticleSystem* sys = fx.GetSystem(0);
    REQUIRE(sys != nullptr);

    // Continuous emission at the seeded rate.
    CHECK(sys->emitter.mode == particles::EmissionMode::Continuous);
    CHECK(sys->emitter.spawnRate == doctest::Approx(120.0f));

    // The modules the property grid edits are all present.
    auto* life = static_cast<particles::LifetimeInitializer*>(nullptr);
    auto* vel = static_cast<particles::VelocityInitializer*>(nullptr);
    auto* size = static_cast<particles::SizeInitializer*>(nullptr);
    auto* color = static_cast<particles::ColorInitializer*>(nullptr);
    for (i32 i = 0; i < sys->InitializerCount(); ++i)
    {
        if (auto* p = Cast<particles::LifetimeInitializer>(sys->GetInitializer(i)))
            life = p;
        if (auto* p = Cast<particles::VelocityInitializer>(sys->GetInitializer(i)))
            vel = p;
        if (auto* p = Cast<particles::SizeInitializer>(sys->GetInitializer(i)))
            size = p;
        if (auto* p = Cast<particles::ColorInitializer>(sys->GetInitializer(i)))
            color = p;
    }
    CHECK(life != nullptr);
    CHECK(vel != nullptr);
    CHECK(size != nullptr);
    CHECK(color != nullptr);
    if (life != nullptr)
    {
        CHECK(life->lifetime.min == doctest::Approx(1.5f));
        CHECK(life->lifetime.max == doctest::Approx(2.5f));
    }

    particles::GravityBehavior* grav = nullptr;
    for (i32 i = 0; i < sys->BehaviorCount(); ++i)
        if (auto* p = Cast<particles::GravityBehavior>(sys->GetBehavior(i)))
            grav = p;
    CHECK(grav != nullptr);
}
