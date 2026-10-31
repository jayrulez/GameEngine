// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The replicated-spawn resolver's body: a prefab id becomes a live prefab spawned from the content
// database. GameInstanceTests pins the wiring (the controller applies the resolver to every
// endpoint) and ReplicationTests the handler contract with a stand-in; neither reaches the body.
#include <doctest/doctest.h>
#include <filesystem>
#include "Core/Prelude.h"
import foundation.core;
import foundation.vfs;
import foundation.content;
import foundation.scene;
import foundation.scene.resource;
import engine.defaultapp;

using namespace foundation::core;
namespace scene = foundation::scene;
namespace content = foundation::content;

namespace
{
    struct PrefabFixture
    {
        std::filesystem::path dir = "scratch_defaultapp_netspawn";
        UniquePtr<foundation::vfs::NativeFileSystem> mount;
        UniquePtr<content::ContentDatabase> db;
        Guid turret;   // a two-entity prefab: Turret, with Barrel under it
        Guid emptyDoc; // an instance with no scene stream

        PrefabFixture()
        {
            std::error_code ec;
            std::filesystem::remove_all(dir, ec);
            std::filesystem::create_directories(dir, ec);
            mount = MakeUnique<foundation::vfs::NativeFileSystem>(
                DefaultAllocator(), u8"scratch_defaultapp_netspawn", DefaultAllocator());
            db = MakeUnique<content::ContentDatabase>(DefaultAllocator(), DefaultAllocator(), *mount,
                                                      BinarySerializerFactory(), u8".rasset");
            // Author the prefab: root and child, captured the way SavePrefab captures.
            scene::Scene author(DefaultAllocator(), u8"author");
            const scene::EntityHandle root = author.CreateEntity(u8"Turret");
            const scene::EntityHandle barrel = author.CreateEntity(u8"Barrel");
            author.SetParent(barrel, root);
            MemoryStream payload;
            REQUIRE(scene::CapturePrefab(author, root, payload).IsOk());
            content::Instance* prefab =
                db->RootGroup()->CreateInstance(u8"Turret", scene::PrefabDocument::StaticType());
            REQUIRE(prefab != nullptr);
            REQUIRE(prefab->WriteData(u8"scene", payload.Bytes()).IsOk());
            turret = prefab->Id();
            content::Instance* bare =
                db->RootGroup()->CreateInstance(u8"Bare", scene::PrefabDocument::StaticType());
            REQUIRE(bare != nullptr);
            emptyDoc = bare->Id();
        }
        ~PrefabFixture()
        {
            db.Reset();
            mount.Reset();
            std::error_code ec;
            std::filesystem::remove_all(dir, ec);
        }
    };
}

TEST_CASE("defaultapp: the net spawn resolver spawns the whole prefab subtree")
{
    PrefabFixture f;
    scene::Scene target(DefaultAllocator(), u8"level");
    const scene::EntityHandle root = engine::runtime::DefaultApplication::ResolveNetworkPrefab(
        f.db.Get(), nullptr, target, f.turret);
    REQUIRE(root.IsAssigned());
    // The child came too: a resolver that spawns the root and drops the subtree passes a bare
    // IsAssigned check and is still wrong on screen.
    CHECK(target.EntityCount() == 2u);
    CHECK(target.GetFirstChild(root).IsAssigned());
}

TEST_CASE("defaultapp: two resolves are two distinct entities, not one aliased twice")
{
    PrefabFixture f;
    scene::Scene target(DefaultAllocator(), u8"level");
    const scene::EntityHandle a = engine::runtime::DefaultApplication::ResolveNetworkPrefab(
        f.db.Get(), nullptr, target, f.turret);
    const scene::EntityHandle b = engine::runtime::DefaultApplication::ResolveNetworkPrefab(
        f.db.Get(), nullptr, target, f.turret);
    REQUIRE(a.IsAssigned());
    REQUIRE(b.IsAssigned());
    CHECK(a != b);
    CHECK(target.EntityCount() == 4u);
}

TEST_CASE("defaultapp: the net spawn resolver answers Invalid on every failure path, spawning nothing")
{
    PrefabFixture f;
    scene::Scene target(DefaultAllocator(), u8"level");
    // No database (the app's content DB is handed over after construction).
    CHECK_FALSE(engine::runtime::DefaultApplication::ResolveNetworkPrefab(nullptr, nullptr, target,
                                                                          f.turret)
                    .IsAssigned());
    // An unknown id (version skew between peers).
    Guid unknown;
    REQUIRE(Guid::TryParse(u8"0f0f0f0f-0f0f-4f0f-8f0f-0f0f0f0f0f0f", unknown));
    CHECK_FALSE(engine::runtime::DefaultApplication::ResolveNetworkPrefab(f.db.Get(), nullptr,
                                                                          target, unknown)
                    .IsAssigned());
    // An instance that exists but carries no scene stream: past the lookup, refused later.
    CHECK_FALSE(engine::runtime::DefaultApplication::ResolveNetworkPrefab(f.db.Get(), nullptr,
                                                                          target, f.emptyDoc)
                    .IsAssigned());
    CHECK(target.EntityCount() == 0u); // never a partial spawn
}
