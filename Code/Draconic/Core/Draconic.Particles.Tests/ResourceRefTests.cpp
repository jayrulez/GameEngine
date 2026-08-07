// Resource-reference layer: ParticleEffectComponent's effectAsset resource::Ref round-trips
// through scene serialization by Guid, resolves through the ResourceManager's proxy handles,
// and the manager clones the cooked effect into a live instance on the next tick.

#include <atomic> // gcc modules: pull in std::atomic's always_inline bodies before any import
                  // (this import mix otherwise fails with "function body not available")
#include <doctest/doctest.h>

#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

import draconic.foundation;
import draconic.vfs;
import draconic.content;
import draconic.resource;
import draconic.particles;
import draconic.particles.resource;
import draconic.engine.particles;
import draconic.scene;
import draconic.scene.resource;

using namespace draconic::foundation;
namespace scene = draconic::scene;
namespace resource = draconic::resource;
namespace particles = draconic::particles;

namespace
{
    void RemoveTree(StringView root)
    {
        draconic::vfs::NativeFileSystem fs(root);
        Array<draconic::vfs::DirEntry> entries;
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
    const StringView dir = u8"draconic_pfxref_test_db";
    RemoveTree(dir);
    (void)CreateDirectory(dir);
    draconic::vfs::NativeFileSystem mount(dir);

    particles::RegisterParticleEffectResource();

    // Cook an effect with one 64-particle system into the content DB.
    draconic::content::ContentDatabase cookedDb(mount, BinarySerializerFactory(), u8".rasset");
    Guid effectId;
    {
        particles::ParticleEffectResource resource;
        particles::ParticleSystem& sys = resource.Effect().AddSystem(64);
        sys.emitter.isEmitting = true;
        draconic::content::Instance* inst = cookedDb.RootGroup()->CreateInstance(
            u8"Puff", particles::ParticleEffectResource::StaticType());
        REQUIRE(inst != nullptr);
        REQUIRE(inst->WriteObject(resource).IsOk());
        effectId = inst->Id();
    }

    resource::ResourceManager resources(cookedDb);
    particles::ParticleEffectFactory factory;
    resources.AddFactory(&factory);

    // Author a scene whose ParticleEffectComponent references the effect BY GUID only.
    MemoryStream blob;
    {
        scene::Scene scene;
        scene.AddSystem<particles::ParticleEffectComponentManager>();
        const scene::EntityHandle e = scene.CreateEntity(u8"Emitter");
        particles::ParticleEffectComponent& c =
            scene.GetSystem<particles::ParticleEffectComponentManager>()->Add(e);
        c.effectAsset.SetId(effectId);
        c.lightRange = 7.0f;

        BinarySerializer ar(blob, SerializeMode::Write);
        scene::SerializeScene(ar, scene);
        REQUIRE(ar.IsOk());
    }

    scene::Scene loaded;
    loaded.AddSystem<particles::ParticleEffectComponentManager>();
    REQUIRE(blob.Seek(0, SeekOrigin::Begin) == 0);
    {
        BinarySerializer ar(blob, SerializeMode::Read);
        scene::SerializeScene(ar, loaded);
        REQUIRE(ar.IsOk());
    }

    auto* mgr = loaded.GetSystem<particles::ParticleEffectComponentManager>();
    REQUIRE(mgr != nullptr);
    REQUIRE(mgr->ComponentCount() == 1u);
    particles::ParticleEffectComponent* c = nullptr;
    mgr->ForEach([&](particles::ParticleEffectComponent& pc, scene::EntityHandle) { c = &pc; });
    REQUIRE(c != nullptr);
    CHECK(c->effectAsset.id == effectId);
    CHECK(c->effectAsset.Get() == nullptr);
    CHECK(c->lightRange == doctest::Approx(7.0f));

    scene::ResolveSceneResources(loaded, resources);
    particles::ParticleEffectResource* live = c->effectAsset.Get();
    REQUIRE(live != nullptr);
    CHECK(live->Effect().SystemCount() == 1);

    // The manager attaches (clones + instantiates) on the next tick: the component gets its OWN
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
    const StringView dir = u8"draconic_pfxref_test_db2";
    RemoveTree(dir);
    (void)CreateDirectory(dir);
    draconic::vfs::NativeFileSystem mount(dir);

    particles::RegisterParticleEffectResource();
    draconic::content::ContentDatabase cookedDb(mount, BinarySerializerFactory(), u8".rasset");
    Guid effectId;
    {
        particles::ParticleEffectResource resource;
        (void)resource.Effect().AddSystem(16);
        draconic::content::Instance* inst = cookedDb.RootGroup()->CreateInstance(
            u8"Spark", particles::ParticleEffectResource::StaticType());
        REQUIRE(inst != nullptr);
        REQUIRE(inst->WriteObject(resource).IsOk());
        effectId = inst->Id();
    }
    resource::ResourceManager resources(cookedDb);
    particles::ParticleEffectFactory factory;
    resources.AddFactory(&factory);

    particles::ParticleEffectComponent c;
    c.SetEffect(resources.Bind<particles::ParticleEffectResource>(effectId));
    REQUIRE(c.instance.Get() != nullptr);
    REQUIRE(c.effectAsset.Get() != nullptr);
    CHECK(c.attachedResource == c.effectAsset.Get());

    RemoveTree(dir);
}
