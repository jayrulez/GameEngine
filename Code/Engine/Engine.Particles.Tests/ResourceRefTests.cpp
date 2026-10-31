// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Resource-reference layer: ParticleEffectComponent's effectAsset resource::Ref round-trips
// through scene serialization by Guid, resolves through the ResourceManager's proxy handles,
// and the manager clones the cooked effect into a live instance on the next tick.

#include <atomic> // gcc modules: pull in std::atomic's always_inline bodies before any import
                  // (this import mix otherwise fails with "function body not available")
#include <doctest/doctest.h>

#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

import foundation.core;
import foundation.vfs;
import foundation.content;
import foundation.resource;
import foundation.particles;
import foundation.particles.resource;
import engine.particles;
import engine.render; // CameraComponentManager (the primary camera the manager reads)
import foundation.scene;
import foundation.scene.resource;
import foundation.render; // ExtractedScene (what the manager extracts into)

using namespace foundation::core;
using namespace engine::particles;
namespace scene = foundation::scene;
namespace resource = foundation::resource;
namespace particles = foundation::particles;

namespace
{
    void RemoveTree(StringView root)
    {
        foundation::vfs::NativeFileSystem fs(root, foundation::core::DefaultAllocator());
        Array<foundation::vfs::DirEntry> entries;
        if (fs.AsEnumerable()->Enumerate(u8"", entries).IsOk())
        {
            for (const auto& e : entries)
            {
                if (!e.isDirectory)
                {
                    (void)fs.AsWritable()->Delete(e.name.AsView());
                }
            }
        }
        (void)RemoveDirectory(root);
    }
}

TEST_CASE("resource-ref: scene round-trip resolves the effect ref and the manager attaches it")
{
    const StringView dir = u8"scratch_pfxref_test_db";
    RemoveTree(dir);
    (void)CreateDirectory(dir);
    foundation::vfs::NativeFileSystem mount(dir, foundation::core::DefaultAllocator());

    particles::RegisterParticleEffectResource();

    // Cook an effect with one 64-particle system into the content DB.
    foundation::content::ContentDatabase cookedDb(foundation::core::DefaultAllocator(), mount, BinarySerializerFactory(), u8".rasset");
    Guid effectId;
    {
        particles::ParticleEffectResource resource;
        particles::ParticleSystem& sys = resource.Effect().AddSystem(64);
        sys.emitter.isEmitting = true;
        foundation::content::Instance* inst = cookedDb.RootGroup()->CreateInstance(
            u8"Puff", particles::ParticleEffectResource::StaticType());
        REQUIRE(inst != nullptr);
        REQUIRE(inst->WriteObject(resource).IsOk());
        effectId = inst->Id();
    }

    resource::ResourceManager resources(DefaultAllocator(), cookedDb);
    particles::ParticleEffectFactory factory;
    resources.AddFactory(&factory);

    // Author a scene whose ParticleEffectComponent references the effect by guid only.
    MemoryStream blob;
    {
        scene::Scene scene{DefaultAllocator()};
        scene.AddSystem<engine::particles::ParticleEffectComponentManager>();
        const scene::EntityHandle e = scene.CreateEntity(u8"Emitter");
        engine::particles::ParticleEffectComponent& c =
            scene.GetSystem<engine::particles::ParticleEffectComponentManager>()->Add(e);
        c.effectAsset.SetId(effectId);
        c.lightRange = 7.0f;

        BinarySerializer ar(blob, SerializeMode::Write);
        scene::SerializeScene(ar, scene);
        REQUIRE(ar.IsOk());
    }

    scene::Scene loaded{DefaultAllocator()};
    loaded.AddSystem<engine::particles::ParticleEffectComponentManager>();
    REQUIRE(blob.Seek(0, SeekOrigin::Begin) == 0);
    {
        BinarySerializer ar(blob, SerializeMode::Read);
        scene::SerializeScene(ar, loaded);
        REQUIRE(ar.IsOk());
    }

    auto* mgr = loaded.GetSystem<engine::particles::ParticleEffectComponentManager>();
    REQUIRE(mgr != nullptr);
    REQUIRE(mgr->ComponentCount() == 1u);
    engine::particles::ParticleEffectComponent* c = nullptr;
    mgr->ForEach([&](engine::particles::ParticleEffectComponent& pc, scene::EntityHandle) { c = &pc; });
    REQUIRE(c != nullptr);
    CHECK(c->effectAsset.id == effectId);
    CHECK(c->effectAsset.Get() == nullptr);
    CHECK(c->lightRange == doctest::Approx(7.0f));

    scene::ResolveSceneResources(loaded, resources);
    particles::ParticleEffectResource* live = c->effectAsset.Get();
    REQUIRE(live != nullptr);
    CHECK(live->Effect().SystemCount() == 1);

    // The manager attaches (clones and instantiates) on the next tick: the component gets its own
    // effect clone, not the shared cooked template.
    CHECK(c->instance.Get() == nullptr);
    loaded.Update(1.0f / 60.0f);
    REQUIRE(c->instance.Get() != nullptr);
    REQUIRE(c->ownedEffect.Get() != nullptr);
    CHECK(c->ownedEffect.Get() != &live->Effect());
    CHECK(c->ownedEffect->SystemCount() == 1);
    CHECK(c->attachedResource == live);

    RemoveTree(dir);
}

TEST_CASE("resource-ref: SetEffect(proxy) still attaches immediately (sample path)")
{
    const StringView dir = u8"scratch_pfxref_test_db2";
    RemoveTree(dir);
    (void)CreateDirectory(dir);
    foundation::vfs::NativeFileSystem mount(dir, foundation::core::DefaultAllocator());

    particles::RegisterParticleEffectResource();
    foundation::content::ContentDatabase cookedDb(foundation::core::DefaultAllocator(), mount, BinarySerializerFactory(), u8".rasset");
    Guid effectId;
    {
        particles::ParticleEffectResource resource;
        (void)resource.Effect().AddSystem(16);
        foundation::content::Instance* inst = cookedDb.RootGroup()->CreateInstance(
            u8"Spark", particles::ParticleEffectResource::StaticType());
        REQUIRE(inst != nullptr);
        REQUIRE(inst->WriteObject(resource).IsOk());
        effectId = inst->Id();
    }
    resource::ResourceManager resources(DefaultAllocator(), cookedDb);
    particles::ParticleEffectFactory factory;
    resources.AddFactory(&factory);

    engine::particles::ParticleEffectComponent c;
    c.SetEffect(resources.Bind<particles::ParticleEffectResource>(effectId));
    REQUIRE(c.instance.Get() != nullptr);
    REQUIRE(c.effectAsset.Get() != nullptr);
    CHECK(c.attachedResource == c.effectAsset.Get());

    RemoveTree(dir);
}

TEST_CASE("particles: entity-active - starts-inactive never attaches/emits; toggle freezes the sim")
{
    particles::RegisterParticleEffectResource();

    scene::Scene sceneObj{DefaultAllocator()};
    sceneObj.AddSystem<engine::particles::ParticleEffectComponentManager>();
    const scene::EntityHandle e = sceneObj.CreateEntity(u8"Emitter");
    engine::particles::ParticleEffectComponent& c =
        sceneObj.GetSystem<engine::particles::ParticleEffectComponentManager>()->Add(e);

    RefPtr<particles::ParticleEffectResource> res =
        MakeRef<particles::ParticleEffectResource>(DefaultAllocator());
    particles::ParticleSystem& sys = res->Effect().AddSystem(64);
    sys.emitter.isEmitting = true;
    sys.emitter.spawnRate = 100.0f;
    sys.AddInitializer<particles::LifetimeInitializer>().lifetime =
        particles::RangeFloat{10.0f, 10.0f}; // long-lived so alive counts are stable
    c.effectAsset = res.Get();

    // Starts inactive: the manager never attaches, nothing emits.
    sceneObj.SetActive(e, false);
    sceneObj.Update(0.1f);
    sceneObj.Update(0.1f);
    CHECK(c.instance.Get() == nullptr);

    // Activation: attaches and simulates.
    sceneObj.SetActive(e, true);
    sceneObj.Update(0.1f); // attach tick
    REQUIRE(c.instance.Get() != nullptr);
    sceneObj.Update(0.2f);
    const i32 alive = c.instance->Effect().GetSystem(0)->AliveCount();
    CHECK(alive > 0);

    // Deactivate: live particles freeze (no simulation, no emission, no decay).
    sceneObj.SetActive(e, false);
    sceneObj.Update(0.5f);
    CHECK(c.instance->Effect().GetSystem(0)->AliveCount() == alive);
}

TEST_CASE("particles: the manager takes its camera position from the scene's primary camera")
{
    // The alpha sort and the trail ribbons work from this point; it must follow the scene's primary
    // camera rather than stay at the origin.
    particles::RegisterParticleEffectResource();
    scene::Scene sceneObj{DefaultAllocator()};
    auto* particlesManager = sceneObj.AddSystem<engine::particles::ParticleEffectComponentManager>();
    auto* cameras = sceneObj.AddSystem<engine::render::CameraComponentManager>();

    sceneObj.Update(0.1f);
    CHECK(particlesManager->CameraPosition().x == doctest::Approx(0.0f)); // no camera yet: the origin

    // The manager ticks in PostUpdate and the scene refreshes world matrices after that phase, so
    // the camera (like the emitters' own positions) is read one frame late: two ticks.
    const scene::EntityHandle camEntity = sceneObj.CreateEntity(u8"Camera");
    sceneObj.SetLocalPosition(camEntity, Float3{3, 4, 5});
    engine::render::CameraComponent& cam = cameras->Add(camEntity);
    cam.primary = true;
    sceneObj.Update(0.1f);
    sceneObj.Update(0.1f);
    CHECK(particlesManager->CameraPosition().x == doctest::Approx(3.0f));
    CHECK(particlesManager->CameraPosition().y == doctest::Approx(4.0f));
    CHECK(particlesManager->CameraPosition().z == doctest::Approx(5.0f));

    // It follows the camera.
    sceneObj.SetLocalPosition(camEntity, Float3{-1, 0, 2});
    sceneObj.Update(0.1f);
    sceneObj.Update(0.1f);
    CHECK(particlesManager->CameraPosition().x == doctest::Approx(-1.0f));
    CHECK(particlesManager->CameraPosition().z == doctest::Approx(2.0f));

    // An inactive primary is skipped: nothing else to pick, so the last known point stands.
    sceneObj.SetActive(camEntity, false);
    sceneObj.Update(0.1f);
    sceneObj.Update(0.1f);
    CHECK(particlesManager->CameraPosition().x == doctest::Approx(-1.0f));
}

// An effect on an entity made and placed this frame, the way a spawned prefab is, fires its
// one-shot burst where the entity is: the simulation runs once the transforms are final, not
// before, when a new entity's world position still reads the origin.
TEST_CASE("particles: a spawned one-shot bursts where its entity is")
{
    particles::RegisterParticleEffectResource();

    scene::Scene sceneObj{DefaultAllocator()};
    auto* manager = sceneObj.AddSystem<engine::particles::ParticleEffectComponentManager>();

    RefPtr<particles::ParticleEffectResource> res =
        MakeRef<particles::ParticleEffectResource>(DefaultAllocator());
    particles::ParticleSystem& sys = res->Effect().AddSystem(32);
    sys.AddInitializer<particles::LifetimeInitializer>().lifetime = particles::RangeFloat{5.0f, 5.0f};
    sys.emitter.mode = particles::EmissionMode::Burst;
    sys.emitter.burstCount = 6;

    sceneObj.Update(0.016f); // a scene already running
    const scene::EntityHandle e = sceneObj.CreateEntity(u8"Burst");
    sceneObj.SetLocalPosition(e, Float3{12.0f, 3.0f, -40.0f});
    engine::particles::ParticleEffectComponent& c = manager->Add(e);
    c.effectAsset = res.Get();
    sceneObj.Update(0.016f);

    REQUIRE(c.instance.Get() != nullptr);
    const particles::ParticleSystem* live = c.instance->Effect().GetSystem(0);
    REQUIRE(live->AliveCount() == 6); // the one burst, on the first step
    for (i32 i = 0; i < 6; ++i)
    {
        CHECK(Length((*live->Streams().Positions())[i] - Float3{12.0f, 3.0f, -40.0f}) < 0.1f);
    }
}

TEST_CASE("particles: the simulated (sRGB) colour reaches billboards and lights decoded to linear")
{
    // An effect's colours are authored sRGB like every colour; they are simulated as entered and
    // decoded where they leave the simulation for the renderer.
    particles::RegisterParticleEffectResource();
    scene::Scene sceneObj{DefaultAllocator()};
    auto* manager = sceneObj.AddSystem<engine::particles::ParticleEffectComponentManager>();

    RefPtr<particles::ParticleEffectResource> res =
        MakeRef<particles::ParticleEffectResource>(DefaultAllocator());
    particles::ParticleSystem& sys = res->Effect().AddSystem(8);
    sys.AddInitializer<particles::LifetimeInitializer>().lifetime = particles::RangeFloat{5.0f, 5.0f};
    sys.AddInitializer<particles::ColorInitializer>().color =
        particles::RangeColor::Constant(Float4{0.5f, 0.25f, 1.0f, 1.0f});
    sys.renderMode = particles::ParticleRenderMode::Light; // a light per particle, and its glow
    sys.emitter.mode = particles::EmissionMode::Burst;
    sys.emitter.burstCount = 2;

    const scene::EntityHandle e = sceneObj.CreateEntity(u8"Glow");
    engine::particles::ParticleEffectComponent& c = manager->Add(e);
    c.effectAsset = res.Get();
    sceneObj.Update(0.016f);
    REQUIRE(c.instance.Get() != nullptr);

    foundation::render::ExtractedScene snapshot{DefaultAllocator()};
    manager->ExtractRenderData(snapshot);
    REQUIRE(snapshot.Lights().Size() == 2u);
    CHECK(snapshot.Lights()[0].color.x == doctest::Approx(SrgbToLinear(0.5f)));
    CHECK(snapshot.Lights()[0].color.y == doctest::Approx(SrgbToLinear(0.25f)));
    CHECK(snapshot.Lights()[0].color.z == doctest::Approx(1.0f));

    REQUIRE(snapshot.Items().Size() == 1u); // the billboard batch
    const auto* batch =
        static_cast<const engine::particles::ParticleBillboardRenderData*>(snapshot.Items()[0]);
    REQUIRE(batch->count == 2u);
    CHECK(batch->instances[0].color.x == doctest::Approx(SrgbToLinear(0.5f)));
    CHECK(batch->instances[0].color.w == doctest::Approx(1.0f)); // alpha is not decoded

    // The helper the four upload points share.
    const Float4 l = ParticleColorToLinear(Float4{0.5f, 0.0f, 1.0f, 0.25f});
    CHECK(l.x == doctest::Approx(SrgbToLinear(0.5f)));
    CHECK(l.w == doctest::Approx(0.25f));
}
