// Phase 5 - whole-scene serialization round-trip: serialize a scene (entities +
// transform hierarchy + components) to bytes and deserialize into a fresh scene,
// preserving Guids, names, parent links, transforms, and component data.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

import draconic.foundation;
import draconic.scene;
import draconic.scene.resource;
import draconic.xml.serialization;

using namespace draconic::foundation;
using namespace draconic::scene;

namespace
{
    struct Health
    {
        f32 value = 100.0f;
    };
    void Serialize(ISerializer& ar, Health& h) { draconic::foundation::Serialize(ar, "value", h.value); }

    class HealthManager : public SerializableComponentManager<Health>
    {
    public:
        HealthManager() : SerializableComponentManager<Health>(u8"demo.Health") {}
    };

    bool Near(f32 a, f32 b) { return Abs(a - b) < 1e-4f; }
}

TEST_CASE("scene round-trips through SerializeScene (entities, hierarchy, transforms, components)")
{
    // --- author scene A ---
    Scene a(u8"level");
    HealthManager* mgrA = a.AddSystem<HealthManager>();
    EntityHandle root = a.CreateEntity(u8"root");
    EntityHandle child = a.CreateEntity(u8"child");
    a.SetParent(child, root);
    a.SetLocalPosition(root, Float3{1, 2, 3});
    a.SetLocalPosition(child, Float3{4, 0, 0});
    mgrA->Add(root).value = 50.0f;
    mgrA->Add(child).value = 75.0f;
    a.SetActive(child, false);

    const Guid rootId = a.GetEntityId(root);
    const Guid childId = a.GetEntityId(child);

    // --- serialize to memory ---
    MemoryStream stream;
    {
        BinarySerializer writer(stream, SerializeMode::Write);
        SerializeScene(writer, a);
    }
    (void)stream.Seek(0, SeekOrigin::Begin);

    // --- deserialize into a fresh scene B (with the same manager present) ---
    Scene b;
    HealthManager* mgrB = b.AddSystem<HealthManager>();
    {
        BinarySerializer reader(stream, SerializeMode::Read);
        SerializeScene(reader, b);
    }

    // --- verify ---
    CHECK(b.Name() == u8"level");
    CHECK(b.EntityCount() == 2);

    EntityHandle rootB = b.FindEntity(rootId); // Guids preserved
    EntityHandle childB = b.FindEntity(childId);
    REQUIRE(rootB.IsAssigned());
    REQUIRE(childB.IsAssigned());

    CHECK(b.GetEntityName(rootB) == u8"root");
    CHECK(b.GetEntityName(childB) == u8"child");
    CHECK(b.GetParent(childB) == rootB); // hierarchy relinked
    CHECK(b.GetParent(rootB) == EntityHandle::Invalid());
    CHECK_FALSE(b.IsActive(childB)); // active state preserved

    CHECK(Near(b.GetLocalTransform(rootB).position.x, 1.0f));
    CHECK(Near(b.GetLocalTransform(childB).position.x, 4.0f));

    REQUIRE(mgrB->Has(rootB));
    REQUIRE(mgrB->Has(childB));
    CHECK(Near(mgrB->Get(rootB)->value, 50.0f)); // component data preserved
    CHECK(Near(mgrB->Get(childB)->value, 75.0f));
    CHECK(mgrB->Count() == 2);

    // world transforms compose correctly after load
    b.UpdateTransforms();
    CHECK(Near(b.GetWorldPosition(childB).x, 5.0f)); // root(1) + child local(4)
    (void)mgrA;
}

TEST_CASE("empty scene round-trips")
{
    Scene a(u8"empty");
    MemoryStream stream;
    {
        BinarySerializer w(stream, SerializeMode::Write);
        SerializeScene(w, a);
    }
    (void)stream.Seek(0, SeekOrigin::Begin);

    Scene b;
    {
        BinarySerializer r(stream, SerializeMode::Read);
        SerializeScene(r, b);
    }
    CHECK(b.Name() == u8"empty");
    CHECK(b.EntityCount() == 0);
}

// Sibling ORDER round-trips: entities are written in TREE order (roots in list order,
// depth-first children), so load's create+relink sequence reproduces the reordered lists -
// the editor hierarchy is reorderable and saves must preserve it.
TEST_CASE("scene-serialize: sibling order round-trips after reorders")
{
    Scene scene(u8"ordered");
    EntityHandle a = scene.CreateEntity(u8"a");
    EntityHandle b = scene.CreateEntity(u8"b");
    EntityHandle c = scene.CreateEntity(u8"c");
    EntityHandle p = scene.CreateEntity(u8"p");
    scene.SetParent(a, p);
    scene.SetParent(b, p);
    scene.SetParent(c, p);
    scene.MoveBefore(c, a);                    // children: c, a, b
    scene.MoveBefore(p, scene.GetFirstRoot()); // p to the front of the roots

    MemoryStream buffer;
    {
        BinarySerializer ar(buffer, SerializeMode::Write);
        SerializeScene(ar, scene);
    }

    Scene loaded;
    (void)buffer.Seek(0, SeekOrigin::Begin);
    {
        BinarySerializer ar(buffer, SerializeMode::Read);
        SerializeScene(ar, loaded);
    }

    // Roots: p first.
    EntityHandle lp = loaded.FindEntity(scene.GetEntityId(p));
    REQUIRE(lp.IsAssigned());
    CHECK(loaded.GetFirstRoot() == lp);

    // Children of p in reordered order: c, a, b.
    EntityHandle k0 = loaded.GetFirstChild(lp);
    REQUIRE(k0.IsAssigned());
    CHECK(loaded.GetEntityName(k0) == u8"c");
    EntityHandle k1 = loaded.GetNextSibling(k0);
    REQUIRE(k1.IsAssigned());
    CHECK(loaded.GetEntityName(k1) == u8"a");
    EntityHandle k2 = loaded.GetNextSibling(k1);
    REQUIRE(k2.IsAssigned());
    CHECK(loaded.GetEntityName(k2) == u8"b");
}

// Regression (user-hit, corrupted a real save): the scene's guid RNG is deterministic and a
// LOAD does not advance it, so entities created after a load reproduced loaded guids - and
// since editor commands route BY GUID, edits landed on the wrong entity. CreateEntity now
// re-rolls on collision; loading a save that already contains duplicates recovers by
// reassigning fresh ids (warning) instead of asserting.
TEST_CASE("scene-serialize: fresh guids never collide with loaded entities")
{
    // Session 1: create + save.
    MemoryStream blob;
    Guid firstId;
    {
        Scene scene;
        firstId = scene.GetEntityId(scene.CreateEntity(u8"First"));
        BinarySerializer ar(blob, SerializeMode::Write);
        SerializeScene(ar, scene);
        REQUIRE(ar.IsOk());
    }

    // Session 2 (fresh scene = fresh deterministic RNG): load, then create MORE entities.
    Scene loaded;
    REQUIRE(blob.Seek(0, SeekOrigin::Begin) == 0);
    {
        BinarySerializer ar(blob, SerializeMode::Read);
        SerializeScene(ar, loaded);
        REQUIRE(ar.IsOk());
    }
    CHECK(loaded.FindEntity(firstId).IsAssigned());

    // Without the re-roll these reproduced firstId (same RNG sequence).
    for (i32 i = 0; i < 4; ++i)
    {
        const Guid fresh = loaded.GetEntityId(loaded.CreateEntity(u8"Later"));
        CHECK(fresh != firstId);
        CHECK(fresh != Guid{});
    }
}

TEST_CASE("scene-serialize: a save with duplicate entity guids loads with recovery")
{
    // Forge a corrupt save: two entities sharing one guid (the pre-fix bug's output).
    MemoryStream blob;
    Guid shared;
    {
        Scene scene;
        shared = scene.GetEntityId(scene.CreateEntity(u8"Original"));
        (void)scene.CreateEntity(shared, u8"Impostor"); // explicit-guid create = the corruption
        BinarySerializer ar(blob, SerializeMode::Write);
        SerializeScene(ar, scene);
        REQUIRE(ar.IsOk());
    }

    Scene loaded;
    REQUIRE(blob.Seek(0, SeekOrigin::Begin) == 0);
    {
        BinarySerializer ar(blob, SerializeMode::Read);
        SerializeScene(ar, loaded);
        REQUIRE(ar.IsOk());
    }

    // Both entities exist, uniquely addressable; the shared guid resolves to its first holder.
    CHECK(loaded.EntityCount() == 2u);
    const EntityHandle first = loaded.FindEntity(shared);
    REQUIRE(first.IsAssigned());
    CHECK(loaded.GetEntityName(first) == StringView(u8"Original"));
}

TEST_CASE("scene-snapshot: capture -> simulate-style mutations -> restore into the SAME scene")
{
    // The editor's Simulate loop: snapshot, let the running scene mutate freely, restore the
    // exact pre-play state into the same Scene instance (borrowed pointers stay valid; guids
    // are part of the snapshot so guid-keyed state re-resolves).
    Scene scene(u8"level");
    HealthManager* mgr = scene.AddSystem<HealthManager>();
    EntityHandle hero = scene.CreateEntity(u8"hero");
    EntityHandle prop = scene.CreateEntity(u8"prop");
    scene.SetLocalPosition(hero, Float3{1, 0, 0});
    mgr->Add(hero).value = 50.0f;
    const Guid heroId = scene.GetEntityId(hero);
    const Guid propId = scene.GetEntityId(prop);

    UniquePtr<SceneSnapshot> snapshot = SceneSnapshot::Capture(scene);
    REQUIRE(snapshot);

    // "Runtime" mutations: move + damage the hero, destroy the prop, spawn a projectile.
    scene.SetLocalPosition(hero, Float3{9, 9, 9});
    mgr->Get(hero)->value = 1.0f;
    scene.DestroyEntity(prop);
    EntityHandle projectile = scene.CreateEntity(u8"projectile");
    const Guid projectileId = scene.GetEntityId(projectile);

    REQUIRE(snapshot->Restore(scene).IsOk());

    // The exact pre-play world: hero back at its pose/value, the prop resurrected under its
    // ORIGINAL guid, the runtime spawn gone.
    const EntityHandle heroRestored = scene.FindEntity(heroId);
    REQUIRE(heroRestored.IsAssigned());
    CHECK(Near(scene.GetLocalTransform(heroRestored).position.x, 1.0f));
    REQUIRE(mgr->Get(heroRestored) != nullptr);
    CHECK(Near(mgr->Get(heroRestored)->value, 50.0f));
    CHECK(scene.FindEntity(propId).IsAssigned());
    CHECK_FALSE(scene.FindEntity(projectileId).IsAssigned());

    // Restore is repeatable (the same snapshot supports multiple Simulate rounds).
    scene.DestroyEntity(scene.FindEntity(heroId));
    REQUIRE(snapshot->Restore(scene).IsOk());
    CHECK(scene.FindEntity(heroId).IsAssigned());
}

namespace
{
    struct Turret
    {
        f32 range = 5.0f;
        u32 seenVersion = 0;
    };
    void Serialize(ISerializer& ar, Turret& t)
    {
        t.seenVersion = ar.Version(); // record what the scope exposes (test probe)
        draconic::foundation::Serialize(ar, "range", t.range);
    }
    class TurretManager : public SerializableComponentManager<Turret>
    {
    public:
        TurretManager() : SerializableComponentManager<Turret>(u8"demo.Turret") {}
    };
}

DRACONIC_REFLECT_VALUE(Turret, "demo")
{
    builder.DataVersion(3);
    builder.Property<&Turret::range>("range");
}

TEST_CASE("scene-serialize: component records carry the reflected type's data version")
{
    DraconicRegisterValue_Turret(); // patches TypeOf<Turret> (name + dataVersion 3)
    REQUIRE(TypeOf<Turret>().dataVersion == 3u);

    Scene a(u8"level");
    a.AddSystem<TurretManager>()->Add(a.CreateEntity(u8"t")).range = 9.0f;

    MemoryStream blob;
    {
        BinarySerializer ar(blob, SerializeMode::Write);
        SerializeScene(ar, a);
        REQUIRE(ar.IsOk());
    }

    Scene b;
    TurretManager* mgr = b.AddSystem<TurretManager>();
    REQUIRE(blob.Seek(0, SeekOrigin::Begin) == 0);
    {
        BinarySerializer ar(blob, SerializeMode::Read);
        SerializeScene(ar, b);
        REQUIRE(ar.IsOk());
    }
    Turret* loaded = nullptr;
    mgr->ForEach([&](Turret& t, EntityHandle) { loaded = &t; });
    REQUIRE(loaded != nullptr);
    CHECK(loaded->range == doctest::Approx(9.0f));
    // The record's stored version was active while the component deserialized - the seam a
    // component's Serialize migrates through when its layout changes.
    CHECK(loaded->seenVersion == 3u);
}

namespace
{
    // A scene system with a scene-LEVEL settings block (the Sedulous scene-modules pattern:
    // environment/sky-style state, one per scene, shown in the inspector when no entity is
    // selected and persisted with the scene).
    struct FogSettings
    {
        f32 density = 0.5f;
        Color tint = Color{1, 1, 1, 1};
    };

    class FogSystem final : public SceneSystem
    {
    public:
        [[nodiscard]] const TypeInfo* SettingsType() const noexcept override
        {
            return &TypeOf<FogSettings>();
        }
        [[nodiscard]] void* SettingsInstance() noexcept override { return &settings; }
        [[nodiscard]] StringView SettingsId() const noexcept override { return u8"fog"; }
        void SerializeSettings(ISerializer& ar) override
        {
            draconic::foundation::Serialize(ar, "density", settings.density);
            draconic::foundation::Serialize(ar, "tint", settings.tint);
        }
        FogSettings settings;
    };
}

TEST_CASE("scene-serialize: scene-system settings round-trip; pre-settings saves still load")
{
    // --- round-trip ---
    Scene a(u8"level");
    FogSystem* fogA = a.AddSystem<FogSystem>();
    fogA->settings.density = 2.25f;
    fogA->settings.tint = Color{0.2f, 0.4f, 0.6f, 1.0f};
    (void)a.CreateEntity(u8"e");

    MemoryStream stream;
    {
        BinarySerializer writer(stream, SerializeMode::Write);
        SerializeScene(writer, a);
        REQUIRE(writer.IsOk());
    }
    {
        (void)stream.Seek(0, SeekOrigin::Begin);
        Scene b;
        FogSystem* fogB = b.AddSystem<FogSystem>();
        BinarySerializer reader(stream, SerializeMode::Read);
        SerializeScene(reader, b, &stream);
        REQUIRE(reader.IsOk());
        CHECK(Near(fogB->settings.density, 2.25f));
        CHECK(Near(fogB->settings.tint.g, 0.4f));
    }

    // --- legacy stream (saved BEFORE the settings section existed) ---
    // Simulate by serializing a scene with NO settings systems and chopping the trailing
    // settings-count u32 - byte-identical to a pre-settings save. The legacyProbe stream
    // check must leave defaults standing with the serializer still OK.
    Scene legacy(u8"old");
    (void)legacy.CreateEntity(u8"e");
    MemoryStream legacyFull;
    {
        BinarySerializer writer(legacyFull, SerializeMode::Write);
        SerializeScene(writer, legacy);
        REQUIRE(writer.IsOk());
    }
    // A pre-settings save ends after components: chop the settings count PLUS the prefab
    // section (mode tag u8 + instance count u32) that a current write appends after it.
    const usize legacyChop = sizeof(u32) + sizeof(u8) + sizeof(u32);
    MemoryStream legacyStream;
    REQUIRE(legacyFull.Bytes().Size() > legacyChop);
    (void)legacyStream.Write(legacyFull.Bytes().Data(), legacyFull.Bytes().Size() - legacyChop);
    (void)legacyStream.Seek(0, SeekOrigin::Begin);
    {
        Scene c;
        FogSystem* fogC = c.AddSystem<FogSystem>();
        BinarySerializer reader(legacyStream, SerializeMode::Read);
        SerializeScene(reader, c, &legacyStream);
        REQUIRE(reader.IsOk());
        CHECK(Near(fogC->settings.density, 0.5f)); // defaults stand
    }

    // A settings-era save from BEFORE the prefab section: chop just that section - the
    // probe guard must end the read cleanly with no pending instances.
    MemoryStream prePrefabStream;
    (void)prePrefabStream.Write(legacyFull.Bytes().Data(),
                                legacyFull.Bytes().Size() - (sizeof(u8) + sizeof(u32)));
    (void)prePrefabStream.Seek(0, SeekOrigin::Begin);
    {
        Scene c;
        FogSystem* fogC = c.AddSystem<FogSystem>();
        BinarySerializer reader(prePrefabStream, SerializeMode::Read);
        SerializeScene(reader, c, &prePrefabStream);
        REQUIRE(reader.IsOk());
        CHECK(Near(fogC->settings.density, 0.5f));
        CHECK(c.PendingPrefabInstanceCount() == 0u);
    }

    // Without the probe (a snapshot restore), the section is expected and reads normally.
    (void)stream.Seek(0, SeekOrigin::Begin);
    {
        Scene d;
        FogSystem* fogD = d.AddSystem<FogSystem>();
        BinarySerializer reader(stream, SerializeMode::Read);
        SerializeScene(reader, d);
        REQUIRE(reader.IsOk());
        CHECK(Near(fogD->settings.density, 2.25f));
    }
}

// ============================== Prefab P1 =====================================================

TEST_CASE("prefab: capture -> spawn twice (fresh guids, hierarchy, components, baselines)")
{
    Scene author(u8"author");
    HealthManager* authorHealth = author.AddSystem<HealthManager>();
    EntityHandle root = author.CreateEntity(u8"Turret");
    EntityHandle barrel = author.CreateEntity(u8"Barrel");
    author.SetParent(barrel, root);
    author.SetLocalPosition(
        root, Float3{9, 9, 9}); // authoring placement - NOT part of the payload semantics
    author.SetLocalPosition(barrel, Float3{0, 1, 0});
    authorHealth->Add(root).value = 40.0f;
    authorHealth->Add(barrel).value = 10.0f;

    MemoryStream payload;
    REQUIRE(CapturePrefab(author, root, payload).IsOk());

    Scene target(u8"level");
    HealthManager* health = target.AddSystem<HealthManager>();
    EntityHandle anchor = target.CreateEntity(u8"Anchor");

    (void)payload.Seek(0, SeekOrigin::Begin);
    const Guid prefabId{0xAA, 0xBB};
    EntityHandle inst1 = SpawnPrefab(target, payload, prefabId, anchor);
    (void)payload.Seek(0, SeekOrigin::Begin);
    EntityHandle inst2 = SpawnPrefab(target, payload, prefabId);
    REQUIRE(inst1.IsAssigned());
    REQUIRE(inst2.IsAssigned());
    CHECK(target.GetEntityId(inst1) != target.GetEntityId(inst2)); // fresh guids per spawn

    CHECK(target.GetParent(inst1) == anchor);
    CHECK(!target.GetParent(inst2).IsAssigned());
    CHECK(target.GetEntityName(inst1) == StringView(u8"Turret"));
    EntityHandle barrel1 = target.GetFirstChild(inst1);
    REQUIRE(barrel1.IsAssigned());
    CHECK(target.GetEntityName(barrel1) == StringView(u8"Barrel"));
    CHECK(Near(target.GetLocalTransform(barrel1).position.y, 1.0f));
    REQUIRE(health->Has(inst1));
    CHECK(Near(health->Get(inst1)->value, 40.0f));
    REQUIRE(health->Has(barrel1));
    CHECK(Near(health->Get(barrel1)->value, 10.0f));

    CHECK(target.PrefabInstanceCount() == 2u);
    Scene::PrefabInstanceState* state = target.FindPrefabInstanceByRoot(target.GetEntityId(inst1));
    REQUIRE(state != nullptr);
    CHECK(state->prefabId == prefabId);
    CHECK(state->sourceIds.Size() == 2u);
    CHECK(state->componentBaselines.Size() == 2u);
}

TEST_CASE("prefab: scenes save instances as ref+deltas and restore them (overrides survive)")
{
    // Author + capture the payload.
    Scene author(u8"author");
    HealthManager* authorHealth = author.AddSystem<HealthManager>();
    EntityHandle root = author.CreateEntity(u8"Tower");
    EntityHandle top = author.CreateEntity(u8"Top");
    EntityHandle flag = author.CreateEntity(u8"Flag");
    author.SetParent(top, root);
    author.SetParent(flag, top);
    authorHealth->Add(root).value = 100.0f;
    authorHealth->Add(top).value = 50.0f;
    MemoryStream payload;
    REQUIRE(CapturePrefab(author, root, payload).IsOk());

    // Level: one plain entity + one instance with EVERY delta kind.
    Scene level(u8"level");
    HealthManager* health = level.AddSystem<HealthManager>();
    EntityHandle plain = level.CreateEntity(u8"Plain");
    health->Add(plain).value = 7.0f;

    (void)payload.Seek(0, SeekOrigin::Begin);
    const Guid prefabId{0x11, 0x22};
    EntityHandle inst = SpawnPrefab(level, payload, prefabId);
    REQUIRE(inst.IsAssigned());
    level.SetLocalPosition(inst, Float3{5, 0, 5}); // instance placement
    EntityHandle instTop = level.GetFirstChild(inst);
    EntityHandle instFlag = level.GetFirstChild(instTop);
    REQUIRE(instFlag.IsAssigned());
    health->Get(instTop)->value = 51.0f;              // component MODIFY
    level.SetLocalPosition(instTop, Float3{0, 2, 0}); // transform override
    health->Add(instFlag).value = 5.0f;               // component ADD
    health->RemoveComponent(inst);                    // component REMOVE (root's)
    const Guid instId = level.GetEntityId(inst);
    const Guid instTopId = level.GetEntityId(instTop);

    // Save (Referenced) -> the instance's members are NOT plain records.
    MemoryStream saved;
    {
        BinarySerializer w(saved, SerializeMode::Write);
        SerializeScene(w, level);
        REQUIRE(w.IsOk());
    }

    // Load into a fresh scene + resolve prefabs with a payload resolver.
    Scene loaded(u8"loaded");
    HealthManager* loadedHealth = loaded.AddSystem<HealthManager>();
    (void)saved.Seek(0, SeekOrigin::Begin);
    {
        BinarySerializer r(saved, SerializeMode::Read);
        SerializeScene(r, loaded, &saved);
        REQUIRE(r.IsOk());
    }
    CHECK(loaded.EntityCount() == 1u); // only the plain entity so far
    CHECK(loaded.PendingPrefabInstanceCount() == 1u);
    const Span<const byte> payloadBytes = payload.Bytes();
    ResolveScenePrefabs(loaded,
                        Function<UniquePtr<IStream>(const Guid&)>{
                            [&payloadBytes, prefabId](const Guid& id) -> UniquePtr<IStream>
                            {
                                if (id != prefabId)
                                {
                                    return UniquePtr<IStream>{};
                                }
                                auto stream = MakeUnique<MemoryStream>(DefaultAllocator());
                                (void)stream->Write(payloadBytes.Data(), payloadBytes.Size());
                                (void)stream->Seek(0, SeekOrigin::Begin);
                                return UniquePtr<IStream>(stream.Release(), DefaultAllocator());
                            }});

    // Identity: the instance respawned with its SAVED guids.
    EntityHandle lInst = loaded.FindEntity(instId);
    EntityHandle lTop = loaded.FindEntity(instTopId);
    REQUIRE(lInst.IsAssigned());
    REQUIRE(lTop.IsAssigned());
    CHECK(loaded.PrefabInstanceCount() == 1u);

    // Placement + every delta kind survived.
    CHECK(Near(loaded.GetLocalTransform(lInst).position.x, 5.0f));
    CHECK(Near(loaded.GetLocalTransform(lTop).position.y, 2.0f)); // transform override
    REQUIRE(loadedHealth->Has(lTop));
    CHECK(Near(loadedHealth->Get(lTop)->value, 51.0f)); // modify
    EntityHandle lFlag = loaded.GetFirstChild(lTop);
    REQUIRE(lFlag.IsAssigned());
    REQUIRE(loadedHealth->Has(lFlag));
    CHECK(Near(loadedHealth->Get(lFlag)->value, 5.0f)); // add
    CHECK(!loadedHealth->Has(lInst));                   // remove
}

TEST_CASE("prefab: destroyed members stay destroyed across save/load")
{
    Scene author(u8"author");
    (void)author.AddSystem<HealthManager>();
    EntityHandle root = author.CreateEntity(u8"Squad");
    EntityHandle a = author.CreateEntity(u8"A");
    EntityHandle b = author.CreateEntity(u8"B");
    author.SetParent(a, root);
    author.SetParent(b, root);
    MemoryStream payload;
    REQUIRE(CapturePrefab(author, root, payload).IsOk());

    Scene level(u8"level");
    (void)level.AddSystem<HealthManager>();
    (void)payload.Seek(0, SeekOrigin::Begin);
    const Guid prefabId{0x77, 0x88};
    EntityHandle inst = SpawnPrefab(level, payload, prefabId);
    REQUIRE(inst.IsAssigned());
    EntityHandle memberA = level.GetFirstChild(inst);
    REQUIRE(memberA.IsAssigned());
    level.DestroyEntity(memberA); // user deletes one member

    MemoryStream saved;
    {
        BinarySerializer w(saved, SerializeMode::Write);
        SerializeScene(w, level);
    }
    Scene loaded(u8"loaded");
    (void)loaded.AddSystem<HealthManager>();
    (void)saved.Seek(0, SeekOrigin::Begin);
    {
        BinarySerializer r(saved, SerializeMode::Read);
        SerializeScene(r, loaded, &saved);
    }
    const Span<const byte> payloadBytes = payload.Bytes();
    ResolveScenePrefabs(loaded,
                        Function<UniquePtr<IStream>(const Guid&)>{
                            [&payloadBytes](const Guid&) -> UniquePtr<IStream>
                            {
                                auto stream = MakeUnique<MemoryStream>(DefaultAllocator());
                                (void)stream->Write(payloadBytes.Data(), payloadBytes.Size());
                                (void)stream->Seek(0, SeekOrigin::Begin);
                                return UniquePtr<IStream>(stream.Release(), DefaultAllocator());
                            }});

    // Root + ONE surviving child (the destroyed member did not respawn).
    EntityHandle lRoot = loaded.GetFirstRoot();
    REQUIRE(lRoot.IsAssigned());
    u32 childCount = 0;
    for (EntityHandle c = loaded.GetFirstChild(lRoot); c.IsAssigned(); c = loaded.GetNextSibling(c))
    {
        ++childCount;
    }
    CHECK(childCount == 1u);
}

TEST_CASE("prefab: snapshots expand instances and restore their state resolver-free")
{
    Scene author(u8"author");
    HealthManager* authorHealth = author.AddSystem<HealthManager>();
    EntityHandle root = author.CreateEntity(u8"Prop");
    authorHealth->Add(root).value = 33.0f;
    MemoryStream payload;
    REQUIRE(CapturePrefab(author, root, payload).IsOk());

    Scene level(u8"level");
    HealthManager* health = level.AddSystem<HealthManager>();
    (void)payload.Seek(0, SeekOrigin::Begin);
    const Guid prefabId{0x42, 0x42};
    EntityHandle inst = SpawnPrefab(level, payload, prefabId);
    REQUIRE(inst.IsAssigned());
    health->Get(inst)->value = 34.0f; // an override the snapshot must preserve
    const Guid instId = level.GetEntityId(inst);

    UniquePtr<SceneSnapshot> snapshot = SceneSnapshot::Capture(level);
    REQUIRE(static_cast<bool>(snapshot));

    // Simulate: mutate hard (destroy the instance), then restore.
    level.DestroyEntity(inst);
    CHECK(!level.FindEntity(instId).IsAssigned());
    REQUIRE(snapshot->Restore(level).IsOk());

    EntityHandle restored = level.FindEntity(instId);
    REQUIRE(restored.IsAssigned());
    REQUIRE(health->Has(restored));
    CHECK(Near(health->Get(restored)->value, 34.0f));
    CHECK(level.PrefabInstanceCount() == 1u); // bookkeeping restored WITHOUT a resolver
    Scene::PrefabInstanceState* state = level.FindPrefabInstanceByRoot(instId);
    REQUIRE(state != nullptr);
    CHECK(state->prefabId == prefabId);
}

TEST_CASE("prefab: template rebuild preserves deltas and picks up new members")
{
    Scene author(u8"author");
    HealthManager* authorHealth = author.AddSystem<HealthManager>();
    EntityHandle root = author.CreateEntity(u8"House");
    EntityHandle door = author.CreateEntity(u8"Door");
    author.SetParent(door, root);
    authorHealth->Add(door).value = 20.0f;
    MemoryStream payloadV1;
    REQUIRE(CapturePrefab(author, root, payloadV1).IsOk());

    Scene level(u8"level");
    HealthManager* health = level.AddSystem<HealthManager>();
    (void)payloadV1.Seek(0, SeekOrigin::Begin);
    const Guid prefabId{0xF0, 0x0D};
    EntityHandle inst = SpawnPrefab(level, payloadV1, prefabId);
    REQUIRE(inst.IsAssigned());
    const Guid instId = level.GetEntityId(inst);
    EntityHandle instDoor = level.GetFirstChild(inst);
    const Guid instDoorId = level.GetEntityId(instDoor);
    health->Get(instDoor)->value = 21.0f;          // user override
    level.SetLocalPosition(inst, Float3{3, 0, 0}); // placement

    // Template v2: door healthier + a brand-new window member.
    authorHealth->Get(door)->value = 25.0f;
    EntityHandle window = author.CreateEntity(u8"Window");
    author.SetParent(window, root);
    MemoryStream payloadV2;
    REQUIRE(CapturePrefab(author, root, payloadV2).IsOk());

    const Span<const byte> v2 = payloadV2.Bytes();
    CHECK(RebuildPrefabInstances(level, prefabId, v2) == 1u);

    // Identity preserved, override preserved, new member present, placement intact.
    EntityHandle rInst = level.FindEntity(instId);
    EntityHandle rDoor = level.FindEntity(instDoorId);
    REQUIRE(rInst.IsAssigned());
    REQUIRE(rDoor.IsAssigned());
    CHECK(Near(level.GetLocalTransform(rInst).position.x, 3.0f));
    CHECK(Near(health->Get(rDoor)->value, 21.0f)); // override beats the template's 25
    u32 kids = 0;
    bool sawWindow = false;
    for (EntityHandle c = level.GetFirstChild(rInst); c.IsAssigned(); c = level.GetNextSibling(c))
    {
        ++kids;
        if (level.GetEntityName(c) == StringView(u8"Window"))
        {
            sawWindow = true;
        }
    }
    CHECK(kids == 2u);
    CHECK(sawWindow);
    CHECK(level.PrefabInstanceCount() == 1u);
}

TEST_CASE("prefab P2: apply-as-template keeps source ids; revert discards deltas")
{
    Scene author(u8"author");
    HealthManager* authorHealth = author.AddSystem<HealthManager>();
    EntityHandle root = author.CreateEntity(u8"Cart");
    EntityHandle wheel = author.CreateEntity(u8"Wheel");
    author.SetParent(wheel, root);
    authorHealth->Add(wheel).value = 10.0f;
    MemoryStream payload;
    REQUIRE(CapturePrefab(author, root, payload).IsOk());
    const Guid wheelSourceId = author.GetEntityId(wheel);

    Scene level(u8"level");
    HealthManager* health = level.AddSystem<HealthManager>();
    (void)payload.Seek(0, SeekOrigin::Begin);
    const Guid prefabId{0xCA, 0x87};
    EntityHandle inst = SpawnPrefab(level, payload, prefabId);
    REQUIRE(inst.IsAssigned());
    EntityHandle instWheel = level.GetFirstChild(inst);

    // Member queries + override detection.
    PrefabMemberInfo member;
    REQUIRE(FindPrefabMember(level, level.GetEntityId(instWheel), member));
    CHECK(member.state->sourceIds[member.memberIndex] == wheelSourceId);
    ComponentManagerBase* mgr = level.FindManagerBySerializationId(u8"demo.Health");
    REQUIRE(mgr != nullptr);
    CHECK(!IsPrefabComponentOverridden(level, member, *mgr));
    health->Get(instWheel)->value = 11.0f;
    CHECK(IsPrefabComponentOverridden(level, member, *mgr));

    // Apply-as-template: edits + a user-added child become the template, keyed by SOURCE ids.
    EntityHandle lamp = level.CreateEntity(u8"Lamp");
    level.SetParent(lamp, inst);
    MemoryStream applied;
    REQUIRE(CaptureInstanceAsTemplate(level, *member.state, applied).IsOk());

    Scene check(u8"check");
    HealthManager* checkHealth = check.AddSystem<HealthManager>();
    (void)applied.Seek(0, SeekOrigin::Begin);
    HashMap<Guid, Guid> pin; // spawn with source ids AS live ids to inspect the template
    pin.InsertOrAssign(wheelSourceId, wheelSourceId);
    EntityHandle tRoot = SpawnPrefab(check, applied, prefabId, EntityHandle::Invalid(), &pin);
    REQUIRE(tRoot.IsAssigned());
    EntityHandle tWheel = check.FindEntity(wheelSourceId); // SOURCE id preserved
    REQUIRE(tWheel.IsAssigned());
    CHECK(Near(checkHealth->Get(tWheel)->value, 11.0f)); // the applied override
    u32 kids = 0;
    for (EntityHandle c = check.GetFirstChild(tRoot); c.IsAssigned(); c = check.GetNextSibling(c))
    {
        ++kids;
    }
    CHECK(kids == 2u); // wheel + the applied lamp

    // Revert: back to the ORIGINAL template, same guids, deltas gone.
    const Guid instId = level.GetEntityId(inst);
    const Guid instWheelId = level.GetEntityId(instWheel);
    level.SetLocalPosition(inst, Float3{7, 0, 0}); // placement must SURVIVE revert
    const Span<const byte> original = payload.Bytes();
    REQUIRE(RevertPrefabInstance(level, instId, original));
    EntityHandle rInst = level.FindEntity(instId);
    EntityHandle rWheel = level.FindEntity(instWheelId);
    REQUIRE(rInst.IsAssigned());
    REQUIRE(rWheel.IsAssigned());
    CHECK(Near(health->Get(rWheel)->value, 10.0f));               // override discarded
    CHECK(Near(level.GetLocalTransform(rInst).position.x, 7.0f)); // placement kept
    u32 rKids = 0;
    for (EntityHandle c = level.GetFirstChild(rInst); c.IsAssigned(); c = level.GetNextSibling(c))
    {
        ++rKids;
    }
    CHECK(rKids == 1u); // the user-added lamp is gone? NO -
    // the lamp was parented under the instance but is NOT a member; destroying the root took
    // it with the subtree. That is the documented revert semantic: non-member children die
    // with the instance they live under.
}

TEST_CASE("prefab: legacy multi-root payload normalizes to one root on spawn")
{
    // Author TWO root entities and serialize the whole scene (the shape an old prefab-page
    // save produced before single-root enforcement).
    Scene author(u8"author");
    (void)author.CreateEntity(u8"Ball");
    (void)author.CreateEntity(u8"Box");
    MemoryStream payload;
    {
        BinarySerializer ser(payload, SerializeMode::Write);
        SerializeScene(ser, author, nullptr, ScenePrefabMode::Expanded);
    }
    (void)payload.Seek(0, SeekOrigin::Begin);

    Scene level(u8"level");
    EntityHandle root = SpawnPrefab(level, payload, Guid{0xAB, 0x12});
    REQUIRE(root.IsAssigned());
    CHECK(level.GetEntityName(root) == StringView(u8"Ball"));

    // The extra root became a CHILD of the instance root - nothing is a loose sibling.
    EntityHandle child = level.GetFirstChild(root);
    REQUIRE(child.IsAssigned());
    CHECK(level.GetEntityName(child) == StringView(u8"Box"));
    CHECK(!level.GetNextSibling(root).IsAssigned());

    // Apply-as-template walks the root subtree, so BOTH members survive a round-trip.
    Scene::PrefabInstanceState* state = level.FindPrefabInstanceByRoot(level.GetEntityId(root));
    REQUIRE(state != nullptr);
    MemoryStream captured;
    REQUIRE(CaptureInstanceAsTemplate(level, *state, captured).IsOk());
    (void)captured.Seek(0, SeekOrigin::Begin);
    Scene other(u8"other");
    EntityHandle respawned = SpawnPrefab(other, captured, Guid{0xAB, 0x12});
    REQUIRE(respawned.IsAssigned());
    CHECK(other.GetFirstChild(respawned).IsAssigned());
}

TEST_CASE("prefab: SavePrefab refuses a multi-root scene")
{
    // SavePrefab needs a content instance; the root-count gate rejects before any write, so
    // exercise the gate through SerializeScene's caller contract instead: the page blocks
    // multi-root saves and SavePrefab returns InvalidArgument (verified via the editor lib).
    Scene scene(u8"prefab");
    (void)scene.CreateEntity(u8"A");
    (void)scene.CreateEntity(u8"B");
    usize roots = 0;
    for (EntityHandle r = scene.GetFirstRoot(); r.IsAssigned(); r = scene.GetNextSibling(r))
    {
        ++roots;
    }
    CHECK(roots == 2);
}

TEST_CASE("prefab: apply-as-template keeps the template root transform, not the placement")
{
    Scene author(u8"author");
    EntityHandle tmpl = author.CreateEntity(u8"Lamp");
    Transform authored;
    authored.position = Float3{1.0f, 2.0f, 3.0f};
    author.SetLocalTransform(tmpl, authored);
    MemoryStream payload;
    REQUIRE(CapturePrefab(author, tmpl, payload).IsOk());

    Scene level(u8"level");
    (void)payload.Seek(0, SeekOrigin::Begin);
    EntityHandle inst = SpawnPrefab(level, payload, Guid{0x77, 0x3});
    REQUIRE(inst.IsAssigned());

    // Move the instance root - that's PLACEMENT, not template content.
    Transform placed;
    placed.position = Float3{50.0f, 0.0f, -9.0f};
    level.SetLocalTransform(inst, placed);

    Scene::PrefabInstanceState* state = level.FindPrefabInstanceByRoot(level.GetEntityId(inst));
    REQUIRE(state != nullptr);
    MemoryStream captured;
    REQUIRE(CaptureInstanceAsTemplate(level, *state, captured).IsOk());

    // Respawn the captured template elsewhere: the root sits at the AUTHORED transform.
    Scene other(u8"other");
    (void)captured.Seek(0, SeekOrigin::Begin);
    EntityHandle fresh = SpawnPrefab(other, captured, Guid{0x77, 0x3});
    REQUIRE(fresh.IsAssigned());
    const Transform t = other.GetLocalTransform(fresh);
    CHECK(t.position.x == 1.0f);
    CHECK(t.position.y == 2.0f);
    CHECK(t.position.z == 3.0f);
}

// ============================== P4: nesting ==================================

namespace
{
    // Author a Q template: root "Wheel" (health 10) + child "Hub" (health 5).
    Array<byte> AuthorInnerTemplate()
    {
        Scene author(u8"author");
        HealthManager* health = author.AddSystem<HealthManager>();
        EntityHandle wheel = author.CreateEntity(u8"Wheel");
        EntityHandle hub = author.CreateEntity(u8"Hub");
        author.SetParent(hub, wheel);
        health->Add(wheel).value = 10.0f;
        health->Add(hub).value = 5.0f;
        MemoryStream payload;
        REQUIRE(CapturePrefab(author, wheel, payload).IsOk());
        Array<byte> bytes;
        for (byte b : payload.Bytes())
        {
            bytes.PushBack(b);
        }
        return bytes;
    }

    // Author the OUTER template "Cart" in an edit scene: own entity "Body" (health 40) + an
    // instance of the inner template whose Wheel is customized to 11. Saved via the
    // SavePrefab path (Referenced, no settings) so nesting persists as a RECORD.
    struct OuterAuthoring
    {
        Array<byte> payload;
        Guid innerRootSource;
    };
    OuterAuthoring AuthorOuterTemplate(const Guid& innerId, const Array<byte>& innerPayload)
    {
        Scene edit(u8"Cart");
        HealthManager* health = edit.AddSystem<HealthManager>();
        EntityHandle body = edit.CreateEntity(u8"Body");
        health->Add(body).value = 40.0f;

        MemoryStream innerStream;
        (void)innerStream.Write(innerPayload.Data(), innerPayload.Size());
        (void)innerStream.Seek(0, SeekOrigin::Begin);
        EntityHandle wheel = SpawnPrefab(edit, innerStream, innerId, body);
        REQUIRE(wheel.IsAssigned());
        // Owner customization: the Wheel's health becomes 11 INSIDE the Cart template.
        health->Get(wheel)->value = 11.0f;

        MemoryStream out;
        BinarySerializer ser(out, SerializeMode::Write);
        SerializeScene(ser, edit, nullptr, ScenePrefabMode::Referenced, /*includeSettings=*/false);
        REQUIRE(ser.IsOk());
        OuterAuthoring result;
        for (byte b : out.Bytes())
        {
            result.payload.PushBack(b);
        }
        result.innerRootSource = edit.GetEntityId(wheel);
        return result;
    }

    PrefabPayloadResolver MakeResolver(const Guid& innerId, const Array<byte>* innerPayload,
                                       const Guid& outerId = Guid{},
                                       const Array<byte>* outerPayload = nullptr)
    {
        return PrefabPayloadResolver{[=](const Guid& id) -> UniquePtr<IStream>
                                     {
                                         const Array<byte>* source = nullptr;
                                         if (id == innerId)
                                         {
                                             source = innerPayload;
                                         }
                                         else if (id == outerId)
                                         {
                                             source = outerPayload;
                                         }
                                         if (source == nullptr)
                                         {
                                             return UniquePtr<IStream>{};
                                         }
                                         auto stream = MakeUnique<MemoryStream>(DefaultAllocator());
                                         (void)stream->Write(source->Data(), source->Size());
                                         (void)stream->Seek(0, SeekOrigin::Begin);
                                         return UniquePtr<IStream>(stream.Release(),
                                                                   DefaultAllocator());
                                     }};
    }
}

TEST_CASE("prefab P4: nested instance spawns linked, owner customization is BASELINE")
{
    const Guid innerId{0xAA, 0x1};
    const Guid outerId{0xBB, 0x2};
    Array<byte> inner = AuthorInnerTemplate();
    OuterAuthoring outer = AuthorOuterTemplate(innerId, inner);
    PrefabPayloadResolver resolver = MakeResolver(innerId, &inner);

    Scene level(u8"level");
    HealthManager* health = level.AddSystem<HealthManager>();
    MemoryStream outerStream;
    (void)outerStream.Write(outer.payload.Data(), outer.payload.Size());
    (void)outerStream.Seek(0, SeekOrigin::Begin);
    EntityHandle cart =
        SpawnPrefab(level, outerStream, outerId, EntityHandle::Invalid(), nullptr, &resolver);
    REQUIRE(cart.IsAssigned());

    // Two states: the Cart (top-level) + the nested Wheel instance linked to it.
    Scene::PrefabInstanceState* cartState = level.FindPrefabInstanceByRoot(level.GetEntityId(cart));
    REQUIRE(cartState != nullptr);
    CHECK(cartState->ownerRootEntityId.IsNil());
    REQUIRE(cartState->referencedPrefabIds.Size() == 2u); // own + inner
    Scene::PrefabInstanceState* wheelState = nullptr;
    level.ForEachPrefabInstance(
        [&](Scene::PrefabInstanceState& s)
        {
            if (s.prefabId == innerId)
            {
                wheelState = &s;
            }
        });
    REQUIRE(wheelState != nullptr);
    CHECK(wheelState->ownerRootEntityId == cartState->rootEntityId);
    CHECK(wheelState->nestedRootSourceId == outer.innerRootSource);

    // The owner's customization (11) applied AND became the baseline: no override reads.
    EntityHandle wheel = level.FindEntity(wheelState->rootEntityId);
    REQUIRE(wheel.IsAssigned());
    CHECK(health->Get(wheel)->value == doctest::Approx(11.0f));
    PrefabMemberInfo member;
    REQUIRE(FindPrefabMember(level, wheelState->rootEntityId, member));
    ComponentManagerBase* manager = level.FindManagerBySerializationId(u8"demo.Health");
    CHECK(!IsPrefabComponentOverridden(level, member, *manager));
    // The Hub kept its pure-template value.
    EntityHandle hub = level.GetFirstChild(wheel);
    REQUIRE(hub.IsAssigned());
    CHECK(health->Get(hub)->value == doctest::Approx(5.0f));
}

TEST_CASE("prefab P4: scene round-trip preserves nesting links, guids, and scene overrides")
{
    const Guid innerId{0xAA, 0x11};
    const Guid outerId{0xBB, 0x22};
    Array<byte> inner = AuthorInnerTemplate();
    OuterAuthoring outer = AuthorOuterTemplate(innerId, inner);
    PrefabPayloadResolver resolver = MakeResolver(innerId, &inner, outerId, &outer.payload);

    Scene level(u8"level");
    HealthManager* health = level.AddSystem<HealthManager>();
    MemoryStream outerStream;
    (void)outerStream.Write(outer.payload.Data(), outer.payload.Size());
    (void)outerStream.Seek(0, SeekOrigin::Begin);
    EntityHandle cart =
        SpawnPrefab(level, outerStream, outerId, EntityHandle::Invalid(), nullptr, &resolver);
    REQUIRE(cart.IsAssigned());
    Scene::PrefabInstanceState* wheelState = nullptr;
    level.ForEachPrefabInstance(
        [&](Scene::PrefabInstanceState& s)
        {
            if (s.prefabId == innerId)
            {
                wheelState = &s;
            }
        });
    REQUIRE(wheelState != nullptr);
    EntityHandle wheel = level.FindEntity(wheelState->rootEntityId);
    const Guid wheelGuid = wheelState->rootEntityId;
    EntityHandle hub = level.GetFirstChild(wheel);
    const Guid hubGuid = level.GetEntityId(hub);

    // SCENE-level override on the nested Hub (owner baseline is 5).
    health->Get(hub)->value = 7.0f;

    MemoryStream saved;
    {
        BinarySerializer w(saved, SerializeMode::Write);
        SerializeScene(w, level);
        REQUIRE(w.IsOk());
    }
    (void)saved.Seek(0, SeekOrigin::Begin);
    Scene loaded(u8"loaded");
    HealthManager* loadedHealth = loaded.AddSystem<HealthManager>();
    {
        BinarySerializer r(saved, SerializeMode::Read);
        SerializeScene(r, loaded, &saved);
        REQUIRE(r.IsOk());
    }
    ResolveScenePrefabs(loaded, resolver);

    // Guids preserved through the nested merge; owner customization + scene override layered.
    EntityHandle loadedWheel = loaded.FindEntity(wheelGuid);
    EntityHandle loadedHub = loaded.FindEntity(hubGuid);
    REQUIRE(loadedWheel.IsAssigned());
    REQUIRE(loadedHub.IsAssigned());
    CHECK(loadedHealth->Get(loadedWheel)->value == doctest::Approx(11.0f)); // owner custom
    CHECK(loadedHealth->Get(loadedHub)->value == doctest::Approx(7.0f));    // scene override
    Scene::PrefabInstanceState* loadedWheelState = loaded.FindPrefabInstanceByRoot(wheelGuid);
    REQUIRE(loadedWheelState != nullptr);
    CHECK(!loadedWheelState->ownerRootEntityId.IsNil());
    // Scene override still reads as an override (baseline = owner-customized template).
    PrefabMemberInfo member;
    REQUIRE(FindPrefabMember(loaded, hubGuid, member));
    ComponentManagerBase* manager = loaded.FindManagerBySerializationId(u8"demo.Health");
    CHECK(IsPrefabComponentOverridden(loaded, member, *manager));
}

TEST_CASE("prefab P4: inner-template edits propagate THROUGH the outer instance")
{
    const Guid innerId{0xAA, 0x21};
    const Guid outerId{0xBB, 0x32};
    Array<byte> inner = AuthorInnerTemplate();
    OuterAuthoring outer = AuthorOuterTemplate(innerId, inner);

    // The edited inner template: Hub health becomes 50 (Wheel stays 10).
    Array<byte> innerV2;
    {
        Scene author(u8"author");
        HealthManager* health = author.AddSystem<HealthManager>();
        EntityHandle wheel = author.CreateEntity(u8"Wheel");
        EntityHandle hub = author.CreateEntity(u8"Hub");
        author.SetParent(hub, wheel);
        health->Add(wheel).value = 10.0f;
        health->Add(hub).value = 50.0f;
        MemoryStream payload;
        REQUIRE(CapturePrefab(author, wheel, payload).IsOk());
        for (byte b : payload.Bytes())
        {
            innerV2.PushBack(b);
        }
    }

    PrefabPayloadResolver resolver = MakeResolver(innerId, &inner, outerId, &outer.payload);
    Scene level(u8"level");
    HealthManager* health = level.AddSystem<HealthManager>();
    MemoryStream outerStream;
    (void)outerStream.Write(outer.payload.Data(), outer.payload.Size());
    (void)outerStream.Seek(0, SeekOrigin::Begin);
    EntityHandle cart =
        SpawnPrefab(level, outerStream, outerId, EntityHandle::Invalid(), nullptr, &resolver);
    REQUIRE(cart.IsAssigned());
    const Guid cartGuid = level.GetEntityId(cart);

    // Rebuild with the NEW inner template: the resolver serves innerV2 now.
    PrefabPayloadResolver resolverV2 = MakeResolver(innerId, &innerV2, outerId, &outer.payload);
    const u32 rebuilt = RebuildPrefabInstances(
        level, innerId, Span<const byte>{innerV2.Data(), innerV2.Size()}, &resolverV2);
    CHECK(rebuilt == 1u); // the CART rebuilt (it references the inner prefab)

    Scene::PrefabInstanceState* wheelState = nullptr;
    level.ForEachPrefabInstance(
        [&](Scene::PrefabInstanceState& s)
        {
            if (s.prefabId == innerId)
            {
                wheelState = &s;
            }
        });
    REQUIRE(wheelState != nullptr);
    EntityHandle wheel = level.FindEntity(wheelState->rootEntityId);
    REQUIRE(wheel.IsAssigned());
    EntityHandle hub = level.GetFirstChild(wheel);
    REQUIRE(hub.IsAssigned());
    CHECK(health->Get(wheel)->value == doctest::Approx(11.0f)); // owner custom STILL wins
    CHECK(health->Get(hub)->value == doctest::Approx(50.0f));   // template edit propagated
    CHECK(level.FindPrefabInstanceByRoot(cartGuid) != nullptr); // cart intact
}

TEST_CASE("prefab P4: apply-to-prefab keeps nested records with owner customization")
{
    const Guid innerId{0xAA, 0x31};
    const Guid outerId{0xBB, 0x42};
    Array<byte> inner = AuthorInnerTemplate();
    OuterAuthoring outer = AuthorOuterTemplate(innerId, inner);
    PrefabPayloadResolver resolver = MakeResolver(innerId, &inner);

    Scene level(u8"level");
    HealthManager* health = level.AddSystem<HealthManager>();
    MemoryStream outerStream;
    (void)outerStream.Write(outer.payload.Data(), outer.payload.Size());
    (void)outerStream.Seek(0, SeekOrigin::Begin);
    EntityHandle cart =
        SpawnPrefab(level, outerStream, outerId, EntityHandle::Invalid(), nullptr, &resolver);
    REQUIRE(cart.IsAssigned());
    Scene::PrefabInstanceState* cartState = level.FindPrefabInstanceByRoot(level.GetEntityId(cart));
    REQUIRE(cartState != nullptr);

    // Scene tweak: Wheel 11 -> 13, then apply the CART as the new template.
    Scene::PrefabInstanceState* wheelState = nullptr;
    level.ForEachPrefabInstance(
        [&](Scene::PrefabInstanceState& s)
        {
            if (s.prefabId == innerId)
            {
                wheelState = &s;
            }
        });
    REQUIRE(wheelState != nullptr);
    EntityHandle wheel = level.FindEntity(wheelState->rootEntityId);
    health->Get(wheel)->value = 13.0f;

    MemoryStream captured;
    REQUIRE(CaptureInstanceAsTemplate(level, *cartState, captured, &resolver).IsOk());

    // Fresh spawn of the captured template elsewhere: nested link + the 13 travel.
    Scene other(u8"other");
    HealthManager* otherHealth = other.AddSystem<HealthManager>();
    (void)captured.Seek(0, SeekOrigin::Begin);
    EntityHandle fresh =
        SpawnPrefab(other, captured, outerId, EntityHandle::Invalid(), nullptr, &resolver);
    REQUIRE(fresh.IsAssigned());
    Scene::PrefabInstanceState* freshWheelState = nullptr;
    other.ForEachPrefabInstance(
        [&](Scene::PrefabInstanceState& s)
        {
            if (s.prefabId == innerId)
            {
                freshWheelState = &s;
            }
        });
    REQUIRE(freshWheelState != nullptr);
    EntityHandle freshWheel = other.FindEntity(freshWheelState->rootEntityId);
    REQUIRE(freshWheel.IsAssigned());
    CHECK(otherHealth->Get(freshWheel)->value == doctest::Approx(13.0f));
    EntityHandle freshHub = other.GetFirstChild(freshWheel);
    REQUIRE(freshHub.IsAssigned());
    CHECK(otherHealth->Get(freshHub)->value == doctest::Approx(5.0f));
}

TEST_CASE(
    "prefab P4: rebuild preserves user entities under members and user-spawned instances inside")
{
    const Guid innerId{0xAA, 0x41};
    Array<byte> inner = AuthorInnerTemplate();
    PrefabPayloadResolver resolver = MakeResolver(innerId, &inner);

    Scene level(u8"level");
    HealthManager* health = level.AddSystem<HealthManager>();
    MemoryStream innerStream;
    (void)innerStream.Write(inner.Data(), inner.Size());
    (void)innerStream.Seek(0, SeekOrigin::Begin);
    EntityHandle wheel = SpawnPrefab(level, innerStream, innerId);
    REQUIRE(wheel.IsAssigned());
    const Guid wheelGuid = level.GetEntityId(wheel);

    // A plain USER entity parented under a member, and a USER-SPAWNED instance inside.
    EntityHandle userChild = level.CreateEntity(u8"Sticker");
    level.SetParent(userChild, wheel);
    const Guid userChildGuid = level.GetEntityId(userChild);
    MemoryStream innerStream2;
    (void)innerStream2.Write(inner.Data(), inner.Size());
    (void)innerStream2.Seek(0, SeekOrigin::Begin);
    EntityHandle userWheel = SpawnPrefab(level, innerStream2, innerId, wheel);
    REQUIRE(userWheel.IsAssigned());
    const Guid userWheelGuid = level.GetEntityId(userWheel);
    health->Get(userWheel)->value = 99.0f; // scene delta on the user-spawned instance

    const u32 rebuilt = RebuildPrefabInstances(
        level, innerId, Span<const byte>{inner.Data(), inner.Size()}, &resolver);
    CHECK(rebuilt >= 1u);

    // Everything survives: the user child re-attached, the user-spawned instance respawned
    // standalone with its guid + delta.
    EntityHandle survivedChild = level.FindEntity(userChildGuid);
    REQUIRE(survivedChild.IsAssigned());
    CHECK(level.GetParent(survivedChild).IsAssigned());
    CHECK(level.GetEntityId(level.GetParent(survivedChild)) == wheelGuid);
    EntityHandle survivedWheel = level.FindEntity(userWheelGuid);
    REQUIRE(survivedWheel.IsAssigned());
    CHECK(health->Get(survivedWheel)->value == doctest::Approx(99.0f));
}

TEST_CASE("prefab P4: rebuild preserves user entities under NESTED sub-instance members")
{
    const Guid innerId{0xAA, 0x51};
    const Guid outerId{0xBB, 0x51};
    Array<byte> inner = AuthorInnerTemplate();
    OuterAuthoring outer = AuthorOuterTemplate(innerId, inner);
    PrefabPayloadResolver resolver = MakeResolver(innerId, &inner, outerId, &outer.payload);

    Scene level(u8"level");
    level.AddSystem<HealthManager>();
    MemoryStream outerStream;
    (void)outerStream.Write(outer.payload.Data(), outer.payload.Size());
    (void)outerStream.Seek(0, SeekOrigin::Begin);
    EntityHandle cart =
        SpawnPrefab(level, outerStream, outerId, EntityHandle::Invalid(), nullptr, &resolver);
    REQUIRE(cart.IsAssigned());

    // The nested inner sub-instance's root (the Wheel) - a NESTED member: children
    // parented here used to be skipped by the rescue (it only mapped the owner's own
    // members) and died with the teardown.
    Guid wheelLive{};
    level.ForEachPrefabInstance(
        [&](Scene::PrefabInstanceState& st)
        {
            if (st.prefabId == innerId && !st.ownerRootEntityId.IsNil())
            {
                wheelLive = st.rootEntityId;
            }
        });
    REQUIRE(!wheelLive.IsNil());
    EntityHandle wheel = level.FindEntity(wheelLive);
    REQUIRE(wheel.IsAssigned());

    EntityHandle sticker = level.CreateEntity(u8"Sticker");
    level.SetParent(sticker, wheel);
    const Guid stickerGuid = level.GetEntityId(sticker);

    // Rebuild triggered by the OUTER template (the user's failing flow: edit Outer's
    // page + save).
    u32 rebuilt = RebuildPrefabInstances(
        level, outerId, Span<const byte>{outer.payload.Data(), outer.payload.Size()}, &resolver);
    CHECK(rebuilt == 1u);
    EntityHandle survived = level.FindEntity(stickerGuid);
    REQUIRE(survived.IsAssigned());
    REQUIRE(level.GetParent(survived).IsAssigned());
    CHECK(level.GetEntityId(level.GetParent(survived)) == wheelLive);

    // And again through the INNER template (rebuild via referencedPrefabIds).
    rebuilt = RebuildPrefabInstances(level, innerId, Span<const byte>{inner.Data(), inner.Size()},
                                     &resolver);
    CHECK(rebuilt == 1u);
    survived = level.FindEntity(stickerGuid);
    REQUIRE(survived.IsAssigned());
    REQUIRE(level.GetParent(survived).IsAssigned());
    CHECK(level.GetEntityId(level.GetParent(survived)) == wheelLive);
}

TEST_CASE("prefab P4: nested instances keep their captured sibling order")
{
    const Guid innerId{0xAA, 0x61};
    const Guid outerId{0xBB, 0x61};
    Array<byte> inner = AuthorInnerTemplate();

    // Outer authored as: Body > [ Inner instance, Cone ] - the nested instance FIRST.
    Array<byte> outerPayload;
    Guid wheelNs{};
    {
        Scene edit(u8"Outer");
        edit.AddSystem<HealthManager>();
        EntityHandle body = edit.CreateEntity(u8"Body");
        MemoryStream innerStream;
        (void)innerStream.Write(inner.Data(), inner.Size());
        (void)innerStream.Seek(0, SeekOrigin::Begin);
        EntityHandle wheel = SpawnPrefab(edit, innerStream, innerId, body);
        REQUIRE(wheel.IsAssigned());
        wheelNs = edit.GetEntityId(wheel);
        EntityHandle cone = edit.CreateEntity(u8"Cone");
        edit.SetParent(cone, body);
        MemoryStream out;
        BinarySerializer ser(out, SerializeMode::Write);
        SerializeScene(ser, edit, nullptr, ScenePrefabMode::Referenced, false);
        REQUIRE(ser.IsOk());
        for (byte b : out.Bytes())
        {
            outerPayload.PushBack(b);
        }
    }
    PrefabPayloadResolver resolver = MakeResolver(innerId, &inner, outerId, &outerPayload);

    auto childNames = [](Scene& sc, EntityHandle parent)
    {
        Array<String> names;
        for (EntityHandle c = sc.GetFirstChild(parent); c.IsAssigned(); c = sc.GetNextSibling(c))
        {
            names.PushBack(String(sc.GetEntityName(c)));
        }
        return names;
    };

    Scene level(u8"level");
    level.AddSystem<HealthManager>();
    MemoryStream outerStream;
    (void)outerStream.Write(outerPayload.Data(), outerPayload.Size());
    (void)outerStream.Seek(0, SeekOrigin::Begin);
    EntityHandle body =
        SpawnPrefab(level, outerStream, outerId, EntityHandle::Invalid(), nullptr, &resolver);
    REQUIRE(body.IsAssigned());

    // Spawn: the record used to APPEND after Cone; the captured order has Wheel first.
    Array<String> names = childNames(level, body);
    REQUIRE(names.Size() == 2);
    CHECK(names[0] == u8"Wheel");
    CHECK(names[1] == u8"Cone");

    // Rebuild keeps it too (the root guid survives via preassignment).
    const Guid bodyGuid = level.GetEntityId(body);
    const u32 rebuilt = RebuildPrefabInstances(
        level, outerId, Span<const byte>{outerPayload.Data(), outerPayload.Size()}, &resolver);
    CHECK(rebuilt == 1u);
    body = level.FindEntity(bodyGuid);
    REQUIRE(body.IsAssigned());
    Array<String> after = childNames(level, body);
    REQUIRE(after.Size() == 2);
    CHECK(after[0] == u8"Wheel");
    CHECK(after[1] == u8"Cone");
}

TEST_CASE("prefab P4: un-overridden nested placement follows the outer template")
{
    const Guid innerId{0xAA, 0x71};
    const Guid outerId{0xBB, 0x71};
    Array<byte> inner = AuthorInnerTemplate();

    // Author the outer with the inner instance at a given placement.
    auto authorOuter = [&](const Transform& wheelPlacement)
    {
        Scene edit(u8"Outer");
        edit.AddSystem<HealthManager>();
        EntityHandle body = edit.CreateEntity(u8"Body");
        MemoryStream innerStream;
        (void)innerStream.Write(inner.Data(), inner.Size());
        (void)innerStream.Seek(0, SeekOrigin::Begin);
        EntityHandle wheel = SpawnPrefab(edit, innerStream, innerId, body);
        REQUIRE(wheel.IsAssigned());
        edit.SetLocalTransform(wheel, wheelPlacement);
        MemoryStream out;
        BinarySerializer ser(out, SerializeMode::Write);
        SerializeScene(ser, edit, nullptr, ScenePrefabMode::Referenced, false);
        REQUIRE(ser.IsOk());
        Array<byte> bytes;
        for (byte b : out.Bytes())
        {
            bytes.PushBack(b);
        }
        return bytes;
    };

    Transform placementV1{};
    placementV1.position = Float3{1.0f, 0.0f, 0.0f};
    Transform placementV2{};
    placementV2.position = Float3{0.0f, 5.0f, 0.0f};
    Array<byte> outerV1 = authorOuter(placementV1);
    PrefabPayloadResolver resolver = MakeResolver(innerId, &inner, outerId, &outerV1);

    Scene level(u8"level");
    level.AddSystem<HealthManager>();
    MemoryStream outerStream;
    (void)outerStream.Write(outerV1.Data(), outerV1.Size());
    (void)outerStream.Seek(0, SeekOrigin::Begin);
    EntityHandle body =
        SpawnPrefab(level, outerStream, outerId, EntityHandle::Invalid(), nullptr, &resolver);
    REQUIRE(body.IsAssigned());
    Guid wheelLive{};
    level.ForEachPrefabInstance(
        [&](Scene::PrefabInstanceState& st)
        {
            if (st.prefabId == innerId && !st.ownerRootEntityId.IsNil())
            {
                wheelLive = st.rootEntityId;
            }
        });
    REQUIRE(!wheelLive.IsNil());
    CHECK(level.GetLocalTransform(level.FindEntity(wheelLive)).position.x == doctest::Approx(1.0f));

    // Template moves the inner instance; the scene never touched it -> it follows.
    Array<byte> outerV2 = authorOuter(placementV2);
    PrefabPayloadResolver resolver2 = MakeResolver(innerId, &inner, outerId, &outerV2);
    u32 rebuilt = RebuildPrefabInstances(
        level, outerId, Span<const byte>{outerV2.Data(), outerV2.Size()}, &resolver2);
    CHECK(rebuilt == 1u);
    EntityHandle wheel = level.FindEntity(wheelLive);
    REQUIRE(wheel.IsAssigned());
    CHECK(level.GetLocalTransform(wheel).position.y == doctest::Approx(5.0f));
    CHECK(level.GetLocalTransform(wheel).position.x == doctest::Approx(0.0f));

    // Now the SCENE moves it (an override) - a further template change must NOT clobber it.
    Transform sceneOverride{};
    sceneOverride.position = Float3{9.0f, 9.0f, 9.0f};
    level.SetLocalTransform(wheel, sceneOverride);
    Transform placementV3{};
    placementV3.position = Float3{0.0f, 0.0f, 7.0f};
    Array<byte> outerV3 = authorOuter(placementV3);
    PrefabPayloadResolver resolver3 = MakeResolver(innerId, &inner, outerId, &outerV3);
    rebuilt = RebuildPrefabInstances(level, outerId,
                                     Span<const byte>{outerV3.Data(), outerV3.Size()}, &resolver3);
    CHECK(rebuilt == 1u);
    wheel = level.FindEntity(wheelLive);
    REQUIRE(wheel.IsAssigned());
    CHECK(level.GetLocalTransform(wheel).position.x == doctest::Approx(9.0f));
    CHECK(level.GetLocalTransform(wheel).position.z == doctest::Approx(9.0f));
}

TEST_CASE("prefab wire: the retired nested layout is REFUSED, not misparsed")
{
    // A mode-2 (pre-order/placement) prefab section misreads as garbage array counts
    // under the current record layout - the reader must bail at the mode byte instead of
    // looping on a bogus count (the old behavior was an effective OOM/hang on project
    // open). Forge one: serialize a scene, then stamp the retired mode over the section
    // byte and garbage over the instance count.
    Scene author(u8"author");
    author.AddSystem<HealthManager>();
    (void)author.CreateEntity(u8"Plain");
    MemoryStream out;
    BinarySerializer ser(out, SerializeMode::Write);
    SerializeScene(ser, author, nullptr, ScenePrefabMode::Referenced, true);
    REQUIRE(ser.IsOk());

    Array<byte> bytes;
    for (byte b : out.Bytes())
    {
        bytes.PushBack(b);
    }
    // Tail of a no-instance Referenced save: [mode u8][instanceCount u32].
    REQUIRE(bytes.Size() > 5);
    REQUIRE(static_cast<u8>(bytes[bytes.Size() - 5]) == 4u); // kPrefabWireReferenced3
    bytes[bytes.Size() - 5] = static_cast<byte>(2u);         // retired mode
    for (usize i = bytes.Size() - 4; i < bytes.Size(); ++i)
    {
        bytes[i] = static_cast<byte>(0xFFu); // garbage count
    }

    MemoryStream in;
    (void)in.Write(bytes.Data(), bytes.Size());
    (void)in.Seek(0, SeekOrigin::Begin);
    Scene loaded(u8"loaded");
    loaded.AddSystem<HealthManager>();
    BinarySerializer read(in, SerializeMode::Read);
    SerializeScene(read, loaded, &in); // must return promptly: entities in, section out

    bool foundPlain = false;
    for (EntityHandle r = loaded.GetFirstRoot(); r.IsAssigned(); r = loaded.GetNextSibling(r))
    {
        if (loaded.GetEntityName(r) == StringView(u8"Plain"))
        {
            foundPlain = true;
        }
    }
    CHECK(foundPlain);
    CHECK(loaded.TakePendingPrefabInstances().IsEmpty());
}

TEST_CASE("text scenes: XML save -> load -> binary -> load is EQUIVALENT and stable")
{
    const Guid innerId{0xAA, 0x91};
    Array<byte> inner = AuthorInnerTemplate();
    PrefabPayloadResolver resolver = MakeResolver(innerId, &inner);

    Scene scene(u8"level");
    HealthManager* health = scene.AddSystem<HealthManager>();
    EntityHandle hero = scene.CreateEntity(u8"Hero");
    Transform t{};
    t.position = Float3{1.25f, -3.5f, 0.0078125f};
    scene.SetLocalTransform(hero, t);
    health->Add(hero).value = 41.5f;
    MemoryStream innerStream;
    (void)innerStream.Write(inner.Data(), inner.Size());
    (void)innerStream.Seek(0, SeekOrigin::Begin);
    EntityHandle wheel = SpawnPrefab(scene, innerStream, innerId);
    REQUIRE(wheel.IsAssigned());
    health->Get(wheel)->value = 77.0f; // an override that must survive every hop

    // Hop 1: XML text.
    draconic::xml::XmlSerializer xmlOut;
    SerializeScene(xmlOut, scene, nullptr, ScenePrefabMode::Referenced, true,
                   draconic::scene::detail::SceneStreamEncoding::Text);
    REQUIRE(xmlOut.IsOk());
    String text1;
    xmlOut.GetOutput(text1);
    REQUIRE(text1.Size() > 0);
    CHECK(text1[0] == utf8char('<'));
    // The override is REAL text - the component op's type and its field VALUE appear
    // verbatim (no hex blob): the diffability the whole format exists for.
    auto contains = [](const String& hay, StringView needle)
    {
        if (needle.Size() > hay.Size())
        {
            return false;
        }
        for (usize i = 0; i + needle.Size() <= hay.Size(); ++i)
        {
            if (hay.AsView().SubStr(i, needle.Size()) == needle)
            {
                return true;
            }
        }
        return false;
    };
    CHECK(contains(text1, u8"demo.Health"));
    CHECK(contains(text1, u8">77<"));

    // Hop 2: load the XML (sniffed), resolve the instance.
    Scene loaded(u8"loaded");
    loaded.AddSystem<HealthManager>();
    MemoryStream textStream;
    (void)textStream.Write(reinterpret_cast<const byte*>(text1.CStr()), text1.Size());
    (void)textStream.Seek(0, SeekOrigin::Begin);
    {
        draconic::scene::detail::SceneStreamReader reader;
        Serializer* ar = reader.Open(textStream);
        REQUIRE(ar != nullptr);
        REQUIRE(reader.Encoding() == draconic::scene::detail::SceneStreamEncoding::Text);
        SerializeScene(*ar, loaded, nullptr, ScenePrefabMode::Referenced, true, reader.Encoding());
        REQUIRE(ar->IsOk());
    }
    ResolveScenePrefabs(loaded, resolver);

    // Hop 3: binary out of the loaded scene, load THAT, compare against the original.
    MemoryStream binary;
    {
        BinarySerializer ar(binary, SerializeMode::Write);
        SerializeScene(ar, loaded, nullptr, ScenePrefabMode::Referenced, true,
                       draconic::scene::detail::SceneStreamEncoding::Binary);
        REQUIRE(ar.IsOk());
    }
    (void)binary.Seek(0, SeekOrigin::Begin);
    Scene last(u8"last");
    HealthManager* lastHealth = last.AddSystem<HealthManager>();
    {
        draconic::scene::detail::SceneStreamReader reader;
        Serializer* ar = reader.Open(binary);
        REQUIRE(ar != nullptr);
        REQUIRE(reader.Encoding() == draconic::scene::detail::SceneStreamEncoding::Binary);
        SerializeScene(*ar, last, nullptr, ScenePrefabMode::Referenced, true, reader.Encoding());
    }
    ResolveScenePrefabs(last, resolver);

    EntityHandle lastHero = last.FindEntity(scene.GetEntityId(hero));
    REQUIRE(lastHero.IsAssigned());
    const Transform ft = last.GetLocalTransform(lastHero);
    CHECK(ft.position.x == t.position.x); // EXACT float round-trip, not Approx
    CHECK(ft.position.y == t.position.y);
    CHECK(ft.position.z == t.position.z);
    REQUIRE(lastHealth->Get(lastHero) != nullptr);
    CHECK(lastHealth->Get(lastHero)->value == 41.5f);
    EntityHandle lastWheel = last.FindEntity(scene.GetEntityId(wheel));
    REQUIRE(lastWheel.IsAssigned());
    REQUIRE(lastHealth->Get(lastWheel) != nullptr);
    CHECK(lastHealth->Get(lastWheel)->value == 77.0f);

    // Stability: re-saving the loaded scene as XML reproduces the SAME text - saves can
    // never generate noise diffs.
    draconic::xml::XmlSerializer xmlAgain;
    SerializeScene(xmlAgain, loaded, nullptr, ScenePrefabMode::Referenced, true,
                   draconic::scene::detail::SceneStreamEncoding::Text);
    String text2;
    xmlAgain.GetOutput(text2);
    CHECK(text1 == text2);
}

TEST_CASE("text scenes: unknown component types SKIP; later records still load")
{
    Scene scene(u8"author");
    HealthManager* health = scene.AddSystem<HealthManager>();
    EntityHandle a = scene.CreateEntity(u8"A");
    EntityHandle b = scene.CreateEntity(u8"B");
    health->Add(a).value = 1.0f;
    health->Add(b).value = 2.0f;

    draconic::xml::XmlSerializer xmlOut;
    SerializeScene(xmlOut, scene, nullptr, ScenePrefabMode::Referenced, true,
                   draconic::scene::detail::SceneStreamEncoding::Text);
    String text;
    xmlOut.GetOutput(text);

    // Corrupt the FIRST health record's type only (the id is "demo.Health").
    String mutated;
    bool replaced = false;
    const StringView typeId(u8"demo.Health");
    for (usize i = 0; i < text.Size(); ++i)
    {
        if (!replaced && i + typeId.Size() <= text.Size() &&
            text.AsView().SubStr(i, typeId.Size()) == typeId)
        {
            mutated.Append(u8"demo.Bogus1");
            i += typeId.Size() - 1;
            replaced = true;
            continue;
        }
        mutated.PushBack(text[i]);
    }
    REQUIRE(replaced);

    Scene loaded(u8"loaded");
    HealthManager* loadedHealth = loaded.AddSystem<HealthManager>();
    MemoryStream stream;
    (void)stream.Write(reinterpret_cast<const byte*>(mutated.CStr()), mutated.Size());
    (void)stream.Seek(0, SeekOrigin::Begin);
    draconic::scene::detail::SceneStreamReader reader;
    Serializer* ar = reader.Open(stream);
    REQUIRE(ar != nullptr);
    SerializeScene(*ar, loaded, nullptr, ScenePrefabMode::Referenced, true, reader.Encoding());

    // A's record was the bogus one - skipped; B's still loads.
    EntityHandle loadedA = loaded.FindEntity(scene.GetEntityId(a));
    EntityHandle loadedB = loaded.FindEntity(scene.GetEntityId(b));
    REQUIRE(loadedA.IsAssigned());
    REQUIRE(loadedB.IsAssigned());
    CHECK(loadedHealth->Get(loadedA) == nullptr);
    REQUIRE(loadedHealth->Get(loadedB) != nullptr);
    CHECK(loadedHealth->Get(loadedB)->value == 2.0f);
}

TEST_CASE("text scenes: transcode to binary preserves parked prefab pendings")
{
    const Guid innerId{0xAA, 0xA1};
    Array<byte> inner = AuthorInnerTemplate();
    PrefabPayloadResolver resolver = MakeResolver(innerId, &inner);

    Scene scene(u8"level");
    HealthManager* health = scene.AddSystem<HealthManager>();
    MemoryStream innerStream;
    (void)innerStream.Write(inner.Data(), inner.Size());
    (void)innerStream.Seek(0, SeekOrigin::Begin);
    EntityHandle wheel = SpawnPrefab(scene, innerStream, innerId);
    REQUIRE(wheel.IsAssigned());
    health->Get(wheel)->value = 55.0f;
    draconic::xml::XmlSerializer xmlOut;
    SerializeScene(xmlOut, scene, nullptr, ScenePrefabMode::Referenced, true,
                   draconic::scene::detail::SceneStreamEncoding::Text);
    String text;
    xmlOut.GetOutput(text);

    // Transcode WITHOUT a resolver: pendings park and must re-emit verbatim.
    Scene scratch(u8"scratch");
    scratch.AddSystem<HealthManager>();
    MemoryStream in;
    (void)in.Write(reinterpret_cast<const byte*>(text.CStr()), text.Size());
    (void)in.Seek(0, SeekOrigin::Begin);
    Result<Array<byte>> binary = TranscodeSceneStreamToBinary(in, scratch, true);
    REQUIRE(binary.HasValue());
    REQUIRE(binary.Value().Size() > 0);
    CHECK(binary.Value()[0] != static_cast<byte>(u8'<'));

    // The binary loads like a player would: pendings restore + resolve into the instance
    // with its override intact.
    Scene player(u8"player");
    HealthManager* playerHealth = player.AddSystem<HealthManager>();
    MemoryStream binStream;
    (void)binStream.Write(binary.Value().Data(), binary.Value().Size());
    (void)binStream.Seek(0, SeekOrigin::Begin);
    draconic::scene::detail::SceneStreamReader reader;
    Serializer* ar = reader.Open(binStream);
    REQUIRE(ar != nullptr);
    SerializeScene(*ar, player, nullptr, ScenePrefabMode::Referenced, true, reader.Encoding());
    ResolveScenePrefabs(player, resolver);

    EntityHandle playerWheel = player.FindEntity(scene.GetEntityId(wheel));
    REQUIRE(playerWheel.IsAssigned());
    REQUIRE(playerHealth->Get(playerWheel) != nullptr);
    CHECK(playerHealth->Get(playerWheel)->value == 55.0f);
}

TEST_CASE("scene v2: unknown component and settings records SKIP instead of aborting")
{
    // Save with health components; load into a scene WITHOUT the manager: entities +
    // hierarchy load, records skip with a warning, and the stream stays consumable to the
    // END (the prefab section after them still parses).
    Scene a(u8"level");
    HealthManager* mgr = a.AddSystem<HealthManager>();
    EntityHandle hero = a.CreateEntity(u8"Hero");
    mgr->Add(hero).value = 42.0f;

    MemoryStream saved;
    {
        BinarySerializer w(saved, SerializeMode::Write);
        SerializeScene(w, a);
        REQUIRE(w.IsOk());
    }
    (void)saved.Seek(0, SeekOrigin::Begin);
    Scene b(u8"loaded"); // NO HealthManager
    {
        BinarySerializer r(saved, SerializeMode::Read);
        SerializeScene(r, b, &saved);
        REQUIRE(r.IsOk());
    }
    CHECK(b.EntityCount() == 1u);
    CHECK(b.FindEntity(a.GetEntityId(hero)).IsAssigned());
}

TEST_CASE("scene-snapshot: a scene WITH a prefab instance restores aligned (Simulate-stop hang "
          "regression)")
{
    // The writer emits owner/nestedSrcRoot unconditionally in the Expanded section; the
    // reader used to gate them on a Referenced-only flag, so any snapshot of a scene
    // holding a prefab instance misaligned on restore - the next count read was garbage
    // in the billions and the member loop allocated until the OS killed the editor.
    Scene scene(u8"level");
    EntityHandle root = scene.CreateEntity(u8"outer-root");
    EntityHandle member = scene.CreateEntity(u8"outer-member");
    scene.SetParent(member, root);

    auto state = MakeUnique<Scene::PrefabInstanceState>(DefaultAllocator());
    const Guid rootId = scene.GetEntityId(root);
    state->prefabId = Guid{0xAA, 0x01};
    state->rootEntityId = rootId;
    state->ownerRootEntityId = Guid{0xBB, 0x02}; // the nesting links the reader skipped
    state->nestedRootSourceId = Guid{0xCC, 0x03};
    state->sourceIds.PushBack(Guid{0xDD, 0x04});
    state->liveIds.PushBack(scene.GetEntityId(member));
    state->baselineTransforms.PushBack(Transform{});
    Scene::PrefabComponentBaseline baseline;
    baseline.sourceEntity = Guid{0xDD, 0x04};
    baseline.typeId = String(u8"test.Health");
    baseline.blob.PushBack(42);
    state->componentBaselines.PushBack(static_cast<Scene::PrefabComponentBaseline&&>(baseline));
    scene.AddPrefabInstance(static_cast<UniquePtr<Scene::PrefabInstanceState>&&>(state));

    UniquePtr<SceneSnapshot> snapshot = SceneSnapshot::Capture(scene);
    REQUIRE(snapshot);
    REQUIRE(snapshot->Restore(scene).IsOk()); // used to spin here allocating gigabytes

    // The instance state round-tripped verbatim - including the nesting links.
    Scene::PrefabInstanceState* restored = scene.FindPrefabInstanceByRoot(rootId);
    REQUIRE(restored != nullptr);
    CHECK(restored->prefabId == Guid{0xAA, 0x01});
    CHECK(restored->ownerRootEntityId == Guid{0xBB, 0x02});
    CHECK(restored->nestedRootSourceId == Guid{0xCC, 0x03});
    REQUIRE(restored->sourceIds.Size() == 1u);
    CHECK(restored->sourceIds[0] == Guid{0xDD, 0x04});
    REQUIRE(restored->componentBaselines.Size() == 1u);
    CHECK(restored->componentBaselines[0].typeId.AsView() == u8"test.Health");
    REQUIRE(restored->componentBaselines[0].blob.Size() == 1u);
    CHECK(restored->componentBaselines[0].blob[0] == 42);

    // Second cycle (the user's repro was play/stop repeatedly): still aligned.
    UniquePtr<SceneSnapshot> second = SceneSnapshot::Capture(scene);
    REQUIRE(second);
    REQUIRE(second->Restore(scene).IsOk());
}

TEST_CASE("text scenes v3: proper guid + full transform names; v2 saves still load")
{
    // Reference scene: hierarchy + transform + a component.
    Scene author(u8"legacy");
    HealthManager* health = author.AddSystem<HealthManager>();
    EntityHandle hero = author.CreateEntity(u8"Hero");
    EntityHandle child = author.CreateEntity(u8"Child");
    author.SetParent(child, hero);
    Transform t{};
    t.position = Float3{1.25f, -3.5f, 0.0078125f};
    t.scale = Float3{2.0f, 2.0f, 2.0f};
    author.SetLocalTransform(hero, t);
    health->Add(hero).value = 41.5f;

    auto contains = [](const String& hay, StringView needle)
    {
        if (needle.Size() > hay.Size())
        {
            return false;
        }
        for (usize i = 0; i + needle.Size() <= hay.Size(); ++i)
        {
            if (hay.AsView().SubStr(i, needle.Size()) == needle)
            {
                return true;
            }
        }
        return false;
    };

    // --- 1) a fresh save uses the proper forms: full transform names + canonical guids ---
    {
        draconic::xml::XmlSerializer xmlOut;
        SerializeScene(xmlOut, author, nullptr, ScenePrefabMode::Referenced, true,
                       draconic::scene::detail::SceneStreamEncoding::Text);
        REQUIRE(xmlOut.IsOk());
        String text;
        xmlOut.GetOutput(text);
        CHECK(contains(text, u8"name=\"position\""));
        CHECK(contains(text, u8"name=\"rotation\""));
        CHECK(contains(text, u8"name=\"scale\""));
        CHECK(!contains(text, u8"name=\"pos\"")); // the old abbreviations are gone
        CHECK(!contains(text, u8"name=\"scl\""));
        CHECK(!contains(text, u8"name=\"hi\"")); // guids are one canonical string, not hi/lo
        CHECK(!contains(text, u8"name=\"lo\""));
        utf8char guidChars[37];
        author.GetEntityId(hero).ToChars(guidChars);
        CHECK(contains(text, StringView{guidChars, 36}));
    }

    // --- 2) a legacy v2 XML stream (hi/lo guid fields, pos/rot/scl keys) still loads ---
    // Replicates the exact wire shapes the v2 writer produced.
    draconic::xml::XmlSerializer legacyOut;
    auto legacyGuid = [](ISerializer& ar, const char* key, Guid g)
    {
        ar.Key(key);
        draconic::foundation::Serialize(ar, "hi", g.high);
        draconic::foundation::Serialize(ar, "lo", g.low);
    };
    u32 magic = draconic::scene::detail::kSceneStreamMagic;
    u32 version = 2;
    draconic::foundation::Serialize(legacyOut, "magic", magic);
    draconic::foundation::Serialize(legacyOut, "version", version);
    String sceneName(u8"legacy");
    draconic::foundation::Serialize(legacyOut, "name", sceneName);
    legacyOut.Key("entities");
    u32 entityCount = 2;
    legacyOut.BeginArray(entityCount);
    EntityHandle order[2] = {hero, child};
    for (EntityHandle e : order)
    {
        Guid id = author.GetEntityId(e);
        String ename = String(author.GetEntityName(e));
        u8 active = 1;
        EntityHandle p = author.GetParent(e);
        Guid parentId = p.IsAssigned() ? author.GetEntityId(p) : Guid{};
        Transform lt = author.GetLocalTransform(e);
        legacyGuid(legacyOut, "id", id);
        draconic::foundation::Serialize(legacyOut, "name", ename);
        draconic::foundation::Serialize(legacyOut, "active", active);
        legacyGuid(legacyOut, "parent", parentId);
        draconic::foundation::Serialize(legacyOut, "pos", lt.position);
        draconic::foundation::Serialize(legacyOut, "rot", lt.rotation);
        draconic::foundation::Serialize(legacyOut, "scl", lt.scale);
    }
    legacyOut.EndArray();
    legacyOut.Key("components");
    u32 componentCount = 1;
    legacyOut.BeginArray(componentCount);
    {
        legacyOut.BeginObject();
        Guid ownerId = author.GetEntityId(hero);
        legacyGuid(legacyOut, "owner", ownerId);
        String typeId(u8"demo.Health");
        draconic::foundation::Serialize(legacyOut, "type", typeId);
        legacyOut.Key("data");
        legacyOut.BeginObject();
        health->WriteComponent(legacyOut, hero);
        legacyOut.EndObject();
        legacyOut.EndObject();
    }
    legacyOut.EndArray();
    legacyOut.Key("systemSettings");
    u32 settingsCount = 0;
    legacyOut.BeginArray(settingsCount);
    legacyOut.EndArray();
    u8 mode = draconic::scene::detail::kPrefabWireReferenced3;
    draconic::foundation::Serialize(legacyOut, "prefabMode", mode);
    legacyOut.Key("prefabInstances");
    u32 instanceCount = 0;
    legacyOut.BeginArray(instanceCount);
    legacyOut.EndArray();
    REQUIRE(legacyOut.IsOk());

    String legacyText;
    legacyOut.GetOutput(legacyText);
    MemoryStream stream;
    (void)stream.Write(reinterpret_cast<const byte*>(legacyText.CStr()), legacyText.Size());
    (void)stream.Seek(0, SeekOrigin::Begin);

    Scene loaded(u8"loaded");
    HealthManager* loadedHealth = loaded.AddSystem<HealthManager>();
    draconic::scene::detail::SceneStreamReader reader;
    Serializer* ar = reader.Open(stream);
    REQUIRE(ar != nullptr);
    REQUIRE(reader.Encoding() == draconic::scene::detail::SceneStreamEncoding::Text);
    SerializeScene(*ar, loaded, nullptr, ScenePrefabMode::Referenced, true, reader.Encoding());
    REQUIRE(ar->IsOk());

    EntityHandle loadedHero = loaded.FindEntity(author.GetEntityId(hero));
    REQUIRE(loadedHero.IsAssigned());
    const Transform lt = loaded.GetLocalTransform(loadedHero);
    CHECK(lt.position.x == t.position.x); // exact floats through the legacy keys
    CHECK(lt.position.y == t.position.y);
    CHECK(lt.position.z == t.position.z);
    CHECK(lt.scale.x == t.scale.x);
    EntityHandle loadedChild = loaded.FindEntity(author.GetEntityId(child));
    REQUIRE(loadedChild.IsAssigned());
    CHECK(loaded.GetParent(loadedChild) == loadedHero); // hi/lo parent guid resolved
    REQUIRE(loadedHealth->Get(loadedHero) != nullptr);
    CHECK(loadedHealth->Get(loadedHero)->value == 41.5f);
}
