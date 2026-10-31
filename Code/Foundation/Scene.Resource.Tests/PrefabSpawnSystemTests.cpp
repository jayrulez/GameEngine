// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// PrefabSpawnSystem: the one runtime spawn recipe (a script's scene.spawn, the replicated spawn)
// as a scene system the host points at its content. Covers placement, the subtree-only bind,
// the refusals, and nesting: a runtime spawn resolves the prefabs its template nests through
// the same database, so those children arrive too.
#include <doctest/doctest.h>
#include <filesystem>
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

import foundation.core;
import foundation.vfs;
import foundation.content;
import foundation.resource;
import foundation.scene;
import foundation.scene.resource;

using namespace foundation::core;
using namespace foundation::scene;
namespace content = foundation::content;

namespace
{
    // A component with a resource-resolve hook that counts, so the subtree-only bind is observable.
    struct Ammo
    {
        i32 rounds = 0;
        i32 binds = 0;                                          // not serialized: spawn-time state
        foundation::resource::ResourceManager* boundThrough = nullptr;
    };
    void Serialize(ISerializer& ar, Ammo& a) { foundation::core::Serialize(ar, "rounds", a.rounds); }
    void ResolveResources(foundation::resource::ResourceManager& manager, Ammo& a)
    {
        ++a.binds;
        a.boundThrough = &manager;
    }
    class AmmoManager : public SerializableComponentManager<Ammo>
    {
    public:
        AmmoManager() : SerializableComponentManager<Ammo>(u8"demo.Ammo") {}
    };

    bool Near(f32 a, f32 b) { return a > b - 0.001f && a < b + 0.001f; }

    // An on-disk content database holding: Turret (root and Barrel child, Ammo 30 on the root),
    // Nest (a root that nests a Turret instance under it), Bare (no scene stream).
    struct PrefabDatabase
    {
        std::filesystem::path dir = "scratch_scene_prefabspawn";
        UniquePtr<foundation::vfs::NativeFileSystem> mount;
        UniquePtr<content::ContentDatabase> db;
        Guid turret;
        Guid nest;
        Guid bare;

        PrefabDatabase()
        {
            std::error_code ec;
            std::filesystem::remove_all(dir, ec);
            std::filesystem::create_directories(dir, ec);
            mount = MakeUnique<foundation::vfs::NativeFileSystem>(
                DefaultAllocator(), u8"scratch_scene_prefabspawn", DefaultAllocator());
            db = MakeUnique<content::ContentDatabase>(DefaultAllocator(), DefaultAllocator(), *mount,
                                                      BinarySerializerFactory(), u8".rasset");
            turret = Store(u8"Turret", AuthorTurret());
            nest = Store(u8"Nest", AuthorNest());
            content::Instance* empty =
                db->RootGroup()->CreateInstance(u8"Bare", PrefabDocument::StaticType());
            REQUIRE(empty != nullptr);
            bare = empty->Id();
        }
        ~PrefabDatabase()
        {
            db.Reset();
            mount.Reset();
            std::error_code ec;
            std::filesystem::remove_all(dir, ec);
        }

        Guid Store(StringView name, const MemoryStream& payload)
        {
            content::Instance* prefab =
                db->RootGroup()->CreateInstance(name, PrefabDocument::StaticType());
            REQUIRE(prefab != nullptr);
            REQUIRE(prefab->WriteData(u8"scene", payload.Bytes()).IsOk());
            return prefab->Id();
        }
        static MemoryStream AuthorTurret()
        {
            Scene author(DefaultAllocator(), u8"author");
            auto* ammo = author.AddSystem<AmmoManager>();
            const EntityHandle root = author.CreateEntity(u8"Turret");
            const EntityHandle barrel = author.CreateEntity(u8"Barrel");
            author.SetParent(barrel, root);
            ammo->Add(root).rounds = 30;
            MemoryStream payload;
            REQUIRE(CapturePrefab(author, root, payload).IsOk());
            return payload;
        }
        MemoryStream AuthorNest()
        {
            // The Nest template holds a live Turret instance under its root, so its capture
            // carries a nesting record that a spawn must resolve through the database.
            Scene author(DefaultAllocator(), u8"author");
            author.AddSystem<AmmoManager>();
            const EntityHandle root = author.CreateEntity(u8"Nest");
            content::Instance* turretInstance = db->GetInstance(turret);
            REQUIRE(turretInstance != nullptr);
            UniquePtr<IStream> turretPayload = turretInstance->ReadData(u8"scene");
            REQUIRE(turretPayload);
            REQUIRE(SpawnPrefab(author, *turretPayload, turret, root).IsAssigned());
            MemoryStream payload;
            REQUIRE(CapturePrefab(author, root, payload).IsOk());
            return payload;
        }
    };

    struct SpawnScene
    {
        Scene scene{DefaultAllocator(), u8"level"};
        AmmoManager* ammo = scene.AddSystem<AmmoManager>();
        PrefabSpawnSystem* spawner = scene.AddSystem<PrefabSpawnSystem>();
    };
}

REFLECT_VALUE(Ammo, "demo")
{
    builder.Property<&Ammo::rounds>("rounds");
}

TEST_CASE("prefab-spawn: a spawn lands where it was asked, with its children, under the parent")
{
    PrefabDatabase db;
    SpawnScene s;
    s.spawner->SetSource(db.db.Get(), nullptr);
    CHECK(s.spawner->HasSource());

    const EntityHandle anchor = s.scene.CreateEntity(u8"anchor");
    s.scene.SetLocalPosition(anchor, Float3{10.0f, 0.0f, 0.0f});
    const Quaternion rotation = Quaternion::FromAxisAngle(Float3{0.0f, 1.0f, 0.0f}, 1.0f);

    const EntityHandle root = s.spawner->Spawn(db.turret, Float3{1.0f, 2.0f, 3.0f}, rotation, anchor);
    REQUIRE(root.IsAssigned());
    CHECK(s.scene.GetEntityName(root) == StringView(u8"Turret"));
    CHECK(s.scene.GetParent(root) == anchor);
    CHECK(s.scene.GetChildCount(root) == 1u); // the Barrel came with it
    const Transform local = s.scene.GetLocalTransform(root);
    CHECK(Near(local.position.x, 1.0f));
    CHECK(Near(local.position.z, 3.0f)); // local to the parent
    CHECK(Near(local.rotation.y, rotation.y));
    s.scene.UpdateTransforms();
    CHECK(Near(s.scene.GetWorldPosition(root).x, 11.0f));

    // A second spawn at a scene root is its own instance; both are recorded on the scene.
    const EntityHandle second = s.spawner->Spawn(db.turret, Float3{0.0f, 0.0f, 0.0f});
    REQUIRE(second.IsAssigned());
    CHECK(second != root);
    CHECK_FALSE(s.scene.GetParent(second).IsAssigned());
    CHECK(s.scene.FindPrefabInstanceByRoot(s.scene.GetEntityId(root)) != nullptr);
    CHECK(s.scene.FindPrefabInstanceByRoot(s.scene.GetEntityId(second)) != nullptr);
}

TEST_CASE("prefab-spawn: a spawn binds its own subtree and nothing else")
{
    PrefabDatabase db;
    SpawnScene s;
    foundation::resource::ResourceManager resources(DefaultAllocator(), *db.db);
    s.spawner->SetSource(db.db.Get(), &resources);

    // A component that was already in the scene, bound or not, is not touched by a spawn.
    const EntityHandle older = s.scene.CreateEntity(u8"older");
    s.ammo->Add(older).rounds = 1;

    const EntityHandle root = s.spawner->Spawn(db.turret, Float3{0.0f, 0.0f, 0.0f});
    REQUIRE(root.IsAssigned());
    REQUIRE(s.ammo->Get(root) != nullptr);
    CHECK(s.ammo->Get(root)->rounds == 30);
    CHECK(s.ammo->Get(root)->binds == 1); // the spawned component was bound...
    CHECK(s.ammo->Get(root)->boundThrough == &resources);
    CHECK(s.ammo->Get(older)->binds == 0); // ...the one already there was not re-walked

    // No manager: the subtree arrives unbound (a later resolve pass binds it).
    s.spawner->SetSource(db.db.Get(), nullptr);
    const EntityHandle unbound = s.spawner->Spawn(db.turret, Float3{0.0f, 0.0f, 0.0f});
    REQUIRE(unbound.IsAssigned());
    CHECK(s.ammo->Get(unbound)->binds == 0);
}

TEST_CASE("prefab-spawn: a runtime spawn resolves the prefabs its template nests")
{
    PrefabDatabase db;
    SpawnScene s;
    s.spawner->SetSource(db.db.Get(), nullptr);

    // Nest > Turret > Barrel: three entities, the inner two from a nested record the
    // database resolves. Without a resolver the spawn would produce only one.
    const EntityHandle root = s.spawner->Spawn(db.nest, Float3{0.0f, 0.0f, 0.0f});
    REQUIRE(root.IsAssigned());
    CHECK(s.scene.EntityCount() == 3u);
    const EntityHandle turret = s.scene.FindChildByName(root, u8"Turret");
    REQUIRE(turret.IsAssigned());
    CHECK(s.scene.FindChildByName(turret, u8"Barrel").IsAssigned());
    CHECK(s.ammo->Get(turret) != nullptr); // the nested instance's components came too
    CHECK(s.ammo->Get(turret)->rounds == 30);
}

TEST_CASE("prefab-spawn: every failure is an invalid handle and nothing half spawned")
{
    PrefabDatabase db;
    SpawnScene s;

    // No source at all (a scene no host has pointed at content).
    CHECK_FALSE(s.spawner->HasSource());
    CHECK_FALSE(s.spawner->Spawn(db.turret, Float3{0.0f, 0.0f, 0.0f}).IsAssigned());

    s.spawner->SetSource(db.db.Get(), nullptr);
    CHECK_FALSE(s.spawner->Spawn(Guid{}, Float3{0.0f, 0.0f, 0.0f}).IsAssigned()); // the nil id
    Guid unknown;
    REQUIRE(Guid::TryParse(u8"0f0f0f0f-0f0f-4f0f-8f0f-0f0f0f0f0f0f", unknown));
    CHECK_FALSE(s.spawner->Spawn(unknown, Float3{0.0f, 0.0f, 0.0f}).IsAssigned()); // unknown
    CHECK_FALSE(s.spawner->Spawn(db.bare, Float3{0.0f, 0.0f, 0.0f}).IsAssigned()); // no stream
    CHECK(s.scene.EntityCount() == 0u);

    // The static recipe answers the same way with no database.
    CHECK_FALSE(PrefabSpawnSystem::SpawnInto(s.scene, nullptr, nullptr, db.turret).IsAssigned());
}
