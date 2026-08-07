// Proves the authoring pipeline end to end (no editor UI): build a ParticleEffect in code, cook it
// into a content-DB ParticleEffectResource, then load it back through the ResourceManager + factory
// and verify the reconstructed effect matches - modules polymorphically rebuilt via reflection - and
// that it still simulates. Mirrors TextureFactoryTests.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"
import draconic.foundation;
import draconic.vfs;
import draconic.content;
import draconic.resource;
import draconic.particles;
import draconic.particles.resource;

using namespace draconic::foundation;
using namespace draconic::vfs;
using namespace draconic::resource;
using namespace draconic::particles;

namespace
{
    void RemoveTree()
    {
        FileDelete(u8"draconic_pfx_db/effect.rasset");
        RemoveDirectory(u8"draconic_pfx_db");
    }

    // A representative two-system effect: a billboard fountain + a mesh burst with a sub-emitter link.
    void BuildEffect(ParticleEffect& fx)
    {
        ParticleSystem& fountain = fx.AddSystem(5000, 0xABCDEF01ull);
        fountain.name = String(u8"fountain");
        fountain.blendMode = ParticleBlendMode::Additive;
        fountain.renderMode = ParticleRenderMode::Billboard;
        fountain.sortParticles = true;
        fountain.softDistance = 1.25f;
        fountain.emitter.mode = EmissionMode::Continuous;
        fountain.emitter.spawnRate = 250.0f;
        fountain.AddInitializer<PositionInitializer>().shape = EmissionShape::Cone(0.4f, 0.35f);
        fountain.AddInitializer<LifetimeInitializer>().lifetime = RangeFloat(1.5f, 2.5f);
        fountain.AddInitializer<VelocityInitializer>().baseVelocity = Float3{0.0f, 9.0f, 0.0f};
        fountain.AddInitializer<ColorInitializer>().color =
            RangeColor(Float4{1, 0.5f, 0.1f, 1}, Float4{1, 0.9f, 0.3f, 1});
        fountain.AddBehavior<GravityBehavior>().multiplier = 1.4f;
        fountain.AddBehavior<AlphaOverLifetimeBehavior>().curve =
            ParticleCurveFloat::FadeOut(1.0f, 0.4f);

        ParticleSystem& sparks = fx.AddSystem(2000);
        sparks.name = String(u8"sparks");
        sparks.emitter.isEmitting = false;
        sparks.AddInitializer<LifetimeInitializer>().lifetime = RangeFloat(0.5f, 1.0f);
        sparks.AddBehavior<CollisionBehavior>().bounce = 0.6f;

        SubEmitterLink link = SubEmitterLink::Default();
        link.trigger = ParticleEventType::OnDeath;
        link.childSystemIndex = 1;
        link.spawnCount = 12;
        fx.AddSubEmitterLink(link);
    }
}

TEST_CASE("particles.pipeline: code effect -> cook -> Bind reconstructs an equivalent effect")
{
    RegisterParticleEffectResource();
    RemoveTree();

    NativeFileSystem mount(u8"draconic_pfx_db");
    Guid id;

    // Cook: author the effect into a ParticleEffectResource record and write it to the DB.
    {
        draconic::content::ContentDatabase db(mount, draconic::foundation::BinarySerializerFactory(),
                                              u8".rasset");
        auto* inst =
            db.RootGroup()->CreateInstance(u8"effect", ParticleEffectResource::StaticType());
        id = inst->Id();

        ParticleEffectResource res;
        BuildEffect(res.Effect());
        REQUIRE(inst->WriteObject(res).IsOk());
    }

    // Load: bind the cooked resource back through the manager + factory.
    draconic::content::ContentDatabase db(mount, draconic::foundation::BinarySerializerFactory(),
                                          u8".rasset");
    ParticleEffectFactory factory;
    ResourceManager manager(db);
    manager.AddFactory(&factory);

    Proxy<ParticleEffectResource> res = manager.Bind<ParticleEffectResource>(id);
    REQUIRE(res);
    ParticleEffect& fx = res->Effect();

    // Structure survived the round trip.
    REQUIRE(fx.SystemCount() == 2);
    CHECK(fx.SubEmitterLinks().Size() == 1);

    ParticleSystem* fountain = fx.GetSystem(0);
    REQUIRE(fountain != nullptr);
    CHECK(fountain->MaxParticles() == 5000);
    CHECK(fountain->Seed() == 0xABCDEF01ull);
    CHECK(fountain->blendMode == ParticleBlendMode::Additive);
    CHECK(fountain->sortParticles == true);
    CHECK(fountain->softDistance == doctest::Approx(1.25f));
    CHECK(fountain->emitter.spawnRate == doctest::Approx(250.0f));
    CHECK(fountain->InitializerCount() == 4);
    CHECK(fountain->BehaviorCount() == 2);
    // A polymorphic module reconstructed with the right concrete type + params.
    CHECK(fountain->GetInitializer(0)->GetType() == &PositionInitializer::StaticType());

    ParticleSystem* sparks = fx.GetSystem(1);
    REQUIRE(sparks != nullptr);
    CHECK(sparks->emitter.isEmitting == false);
    CHECK(sparks->BehaviorCount() == 1);

    // The reconstructed effect still simulates: fountain spawns and ages.
    ParticleEffectInstance inst(fx);
    inst.Update(0.1f);
    CHECK(fountain->AliveCount() > 0);

    RemoveTree();
}
