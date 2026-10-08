// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// engine.navigation scene integration: a baked zone + a MoveEntity agent. At Start the subsystem
// loads the zone and registers the agent; navigate() then steers it, and the Update tick writes
// the steered position back to the entity transform until it arrives.

#include <doctest/doctest.h>
#include "Core/Prelude.h"

#include <cmath>

import foundation.core;
import foundation.vfs;
import foundation.content;
import foundation.resource;
import foundation.scene;
import foundation.navigation;
import foundation.navigation.resource;
import engine.navigation;

using namespace foundation::core;
using namespace foundation::navigation;
using namespace engine::navigation;
namespace scene = foundation::scene;
namespace content = foundation::content;
using foundation::resource::ResourceManager;

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

    void BakeGroundZone(Array<byte>& blob, f32 half = 10.0f)
    {
        Array<Float3> verts;
        Array<u32> indices;
        verts.PushBack(Float3{-half, 0, -half});
        verts.PushBack(Float3{half, 0, -half});
        verts.PushBack(Float3{half, 0, half});
        verts.PushBack(Float3{-half, 0, half});
        const u32 t[] = {0, 3, 2, 0, 2, 1};
        for (u32 i : t)
        {
            indices.PushBack(i);
        }
        REQUIRE(NavigationMeshBuilder::Build(Span<const Float3>{verts.Data(), verts.Size()},
                                             Span<const u32>{indices.Data(), indices.Size()},
                                             NavigationBakeParams{}, blob)
                    .IsOk());
    }
}

TEST_CASE("navigation.scene: a MoveEntity agent navigates across a zone to its target")
{
    RegisterNavigationResource();
    RegisterNavigationComponentReflection();
    RemoveTree(u8"scratch_navscene_db");

    // A content DB holding the cooked zone, bound through the factory.
    foundation::vfs::NativeFileSystem mount(u8"scratch_navscene_db", foundation::core::DefaultAllocator());
    content::ContentDatabase db(DefaultAllocator(), mount, BinarySerializerFactory(), u8".rasset");
    Array<byte> blob;
    BakeGroundZone(blob);
    auto* zoneInstance =
        db.RootGroup()->CreateInstance(u8"zone", NavigationZoneSource::StaticType());
    REQUIRE(zoneInstance != nullptr);
    {
        NavigationZoneSource src;
        src.navMeshBlob.Resize(blob.Size());
        MemCopy(src.navMeshBlob.Data(), blob.Data(), blob.Size());
        REQUIRE(zoneInstance->WriteObject(src).IsOk());
    }
    NavigationZoneFactory factory(DefaultAllocator());
    ResourceManager manager(DefaultAllocator(), db);
    manager.AddFactory(&factory);

    // Scene: nav managers + subsystem, a zone entity at the origin, an agent at (-5,0,0).
    scene::Scene scene(DefaultAllocator(), u8"nav");
    AddNavigationSceneManagers(scene);

    // The scene system carries the debug-draw settings block (physics precedent), default off.
    auto* navSystem = scene.GetSystem<NavigationSceneSystem>();
    REQUIRE(navSystem != nullptr);
    CHECK(navSystem->SettingsType() != nullptr);
    CHECK_FALSE(navSystem->Settings().debugDraw);
    navSystem->Settings().debugDraw = true; // editor/scene-settings would flip this
    CHECK(navSystem->Settings().debugDraw);

    scene::EntityHandle zoneEntity = scene.CreateEntity(u8"zone");
    NavMeshZoneComponent& zoneComp = scene.GetSystem<NavMeshZoneComponentManager>()->Add(zoneEntity);
    zoneComp.extents = Float3{15, 10, 15};
    zoneComp.zone.SetId(zoneInstance->Id());
    zoneComp.zone.Bind(manager);
    REQUIRE(zoneComp.zone.Get() != nullptr);
    REQUIRE(zoneComp.zone.Get()->IsValid());

    scene::EntityHandle agentEntity = scene.CreateEntity(u8"agent");
    scene.SetLocalPosition(agentEntity, Float3{-5, 0, 0});
    NavAgentComponent* agent = &scene.GetSystem<NavAgentComponentManager>()->Add(agentEntity);

    scene.UpdateTransforms();
    scene.Start();
    scene.SetSimulationEnabled(true);

    // Registered with the zone and given a crowd slot.
    CHECK(agent->zoneIndex >= 0);
    CHECK(agent->agentId >= 0);

    // The scene system's crowd/query allocations roll up under the Navigation memory tag.
    CHECK(MemoryTagBytes(RegisterMemoryTag("Navigation")) > 0u);
    CHECK(MemoryTagAllocations(RegisterMemoryTag("Navigation")) > 0u);

    // Steer to the far side and step until arrival (12s @ 30 Hz).
    agent->navigate(5.0f, 0.0f, 0.0f);
    CHECK_FALSE(agent->finished);

    const Float3 start = scene.GetWorldPosition(agentEntity);
    for (int step = 0; step < 360 && !agent->finished; ++step)
    {
        scene.Update(1.0f / 30.0f);
    }

    const Float3 end = scene.GetWorldPosition(agentEntity);
    CHECK(agent->finished);                 // reported arrival
    CHECK(end.x > start.x + 5.0f);           // the entity actually moved across the zone (+x)
    CHECK(std::abs(end.x - 5.0f) < 1.5f);    // ...to near the target
    CHECK(agent->remaining() < 1.0f);

    scene.SetSimulationEnabled(false);
    scene.Stop();
    RemoveTree(u8"scratch_navscene_db");
}

// A zone re-baked while the scene runs (the editor cooks the new navmesh and reloads it under a
// playing scene): the crowd was built over the old navmesh, which the reload parks and later
// frees. The zone rebuilds its crowd over the new one and its agents carry on to where they were
// going. It used to keep stepping the old crowd over the freed navmesh (an editor crash).
TEST_CASE("navigation.scene: a zone's navmesh reloaded mid-run: the agent carries on over the new one")
{
    RegisterNavigationResource();
    RegisterNavigationComponentReflection();
    RemoveTree(u8"scratch_navreload_db");
    foundation::vfs::NativeFileSystem mount(u8"scratch_navreload_db", foundation::core::DefaultAllocator());
    content::ContentDatabase db(DefaultAllocator(), mount, BinarySerializerFactory(), u8".rasset");
    const auto write = [](content::Instance& instance, f32 half)
    {
        Array<byte> blob;
        BakeGroundZone(blob, half);
        NavigationZoneSource src;
        src.navMeshBlob.Resize(blob.Size());
        MemCopy(src.navMeshBlob.Data(), blob.Data(), blob.Size());
        REQUIRE(instance.WriteObject(src).IsOk());
    };
    auto* zoneInstance = db.RootGroup()->CreateInstance(u8"zone", NavigationZoneSource::StaticType());
    REQUIRE(zoneInstance != nullptr);
    write(*zoneInstance, 10.0f);
    NavigationZoneFactory factory(DefaultAllocator());
    ResourceManager manager(DefaultAllocator(), db);
    manager.AddFactory(&factory);

    scene::Scene scene(DefaultAllocator(), u8"nav");
    AddNavigationSceneManagers(scene);
    scene::EntityHandle zoneEntity = scene.CreateEntity(u8"zone");
    NavMeshZoneComponent& zoneComp = scene.GetSystem<NavMeshZoneComponentManager>()->Add(zoneEntity);
    zoneComp.extents = Float3{15, 10, 15};
    zoneComp.zone.SetId(zoneInstance->Id());
    zoneComp.zone.Bind(manager);
    REQUIRE(zoneComp.zone.Get() != nullptr);
    scene::EntityHandle agentEntity = scene.CreateEntity(u8"agent");
    scene.SetLocalPosition(agentEntity, Float3{-5, 0, 0});
    NavAgentComponent* agent = &scene.GetSystem<NavAgentComponentManager>()->Add(agentEntity);
    scene.UpdateTransforms();
    scene.Start();
    scene.SetSimulationEnabled(true);
    REQUIRE(agent->agentId >= 0);

    agent->navigate(5.0f, 0.0f, 0.0f);
    for (int step = 0; step < 45; ++step) // part of the way
    {
        scene.Update(1.0f / 30.0f);
    }
    const f32 midway = scene.GetWorldPosition(agentEntity).x;
    CHECK(midway > -4.0f);
    CHECK_FALSE(agent->finished);

    // Re-baked bigger and reloaded; the old navmesh parked, then released for good.
    const NavigationZoneResource* before = zoneComp.zone.Get();
    write(*zoneInstance, 12.0f);
    REQUIRE(manager.Reload(zoneInstance->Id()));
    REQUIRE(zoneComp.zone.Get() != nullptr);
    CHECK(zoneComp.zone.Get() != before);
    for (int frame = 0; frame < 16; ++frame)
    {
        manager.CollectGarbage();
    }

    for (int step = 0; step < 360 && !agent->finished; ++step)
    {
        scene.Update(1.0f / 30.0f);
    }
    const Float3 end = scene.GetWorldPosition(agentEntity);
    CHECK(agent->finished);
    CHECK(std::abs(end.x - 5.0f) < 1.5f); // where it was going, from where it had got to
    CHECK(end.x > midway);

    scene.SetSimulationEnabled(false);
    scene.Stop();
    RemoveTree(u8"scratch_navreload_db");
}

TEST_CASE("navigation.scene: a SCALED zone entity places the navmesh rigidly (no double-scale)")
{
    // The bake and the runtime both use the scale-free RigidPart frame, so a zone on a
    // scaled entity behaves exactly like the unscaled one - the navmesh's world-unit geometry is
    // PLACED, never warped.
    RegisterNavigationResource();
    RegisterNavigationComponentReflection();
    RemoveTree(u8"scratch_navscene_scaled_db");

    foundation::vfs::NativeFileSystem mount(u8"scratch_navscene_scaled_db", foundation::core::DefaultAllocator());
    content::ContentDatabase db(DefaultAllocator(), mount, BinarySerializerFactory(), u8".rasset");
    Array<byte> blob;
    BakeGroundZone(blob);
    auto* zoneInstance =
        db.RootGroup()->CreateInstance(u8"zone", NavigationZoneSource::StaticType());
    REQUIRE(zoneInstance != nullptr);
    {
        NavigationZoneSource src;
        src.navMeshBlob.Resize(blob.Size());
        MemCopy(src.navMeshBlob.Data(), blob.Data(), blob.Size());
        REQUIRE(zoneInstance->WriteObject(src).IsOk());
    }
    NavigationZoneFactory factory(DefaultAllocator());
    ResourceManager manager(DefaultAllocator(), db);
    manager.AddFactory(&factory);

    scene::Scene scene(DefaultAllocator(), u8"nav-scaled");
    AddNavigationSceneManagers(scene);

    scene::EntityHandle zoneEntity = scene.CreateEntity(u8"zone");
    {
        Transform t;
        t.scale = Float3{2.0f, 2.0f, 2.0f}; // the desync trigger before the rigid frame
        scene.SetLocalTransform(zoneEntity, t);
    }
    NavMeshZoneComponent& zoneComp = scene.GetSystem<NavMeshZoneComponentManager>()->Add(zoneEntity);
    zoneComp.extents = Float3{15, 10, 15};
    zoneComp.zone.SetId(zoneInstance->Id());
    zoneComp.zone.Bind(manager);
    REQUIRE(zoneComp.zone.Get() != nullptr);
    REQUIRE(zoneComp.zone.Get()->IsValid());

    scene::EntityHandle agentEntity = scene.CreateEntity(u8"agent");
    scene.SetLocalPosition(agentEntity, Float3{-5, 0, 0});
    NavAgentComponent* agent = &scene.GetSystem<NavAgentComponentManager>()->Add(agentEntity);

    scene.UpdateTransforms();
    scene.Start();
    scene.SetSimulationEnabled(true);
    REQUIRE(agent->zoneIndex >= 0); // the scaled zone still loaded (rigid frame)

    agent->navigate(5.0f, 0.0f, 0.0f);
    for (int step = 0; step < 360 && !agent->finished; ++step)
    {
        scene.Update(1.0f / 30.0f);
    }
    const Float3 end = scene.GetWorldPosition(agentEntity);
    CHECK(agent->finished);
    CHECK(std::abs(end.x - 5.0f) < 1.5f); // same arrival as the unscaled zone - no double-scale
    CHECK(std::abs(end.y) < 0.5f);        // ...and ON the ground plane, not floated/sunk

    scene.SetSimulationEnabled(false);
    scene.Stop();
    RemoveTree(u8"scratch_navscene_scaled_db");
}

TEST_CASE("navigation.scene: per-agent speed applies live and stopDistance arrives short")
{
    RegisterNavigationResource();
    RegisterNavigationComponentReflection();
    RemoveTree(u8"scratch_navspeed_db");

    foundation::vfs::NativeFileSystem mount(u8"scratch_navspeed_db", foundation::core::DefaultAllocator());
    content::ContentDatabase db(DefaultAllocator(), mount, BinarySerializerFactory(), u8".rasset");
    Array<byte> blob;
    BakeGroundZone(blob);
    auto* zoneInstance =
        db.RootGroup()->CreateInstance(u8"zone", NavigationZoneSource::StaticType());
    REQUIRE(zoneInstance != nullptr);
    {
        NavigationZoneSource src;
        src.navMeshBlob.Resize(blob.Size());
        MemCopy(src.navMeshBlob.Data(), blob.Data(), blob.Size());
        REQUIRE(zoneInstance->WriteObject(src).IsOk());
    }
    NavigationZoneFactory factory(DefaultAllocator());
    ResourceManager manager(DefaultAllocator(), db);
    manager.AddFactory(&factory);

    scene::Scene scene(DefaultAllocator(), u8"nav");
    AddNavigationSceneManagers(scene);
    scene::EntityHandle zoneEntity = scene.CreateEntity(u8"zone");
    NavMeshZoneComponent& zoneComp =
        scene.GetSystem<NavMeshZoneComponentManager>()->Add(zoneEntity);
    zoneComp.extents = Float3{15, 10, 15};
    zoneComp.zone.SetId(zoneInstance->Id());
    zoneComp.zone.Bind(manager);
    REQUIRE(zoneComp.zone.Get() != nullptr);

    // SLOW agent: a lower per-call speed covers less ground in the same steps.
    scene::EntityHandle slowEntity = scene.CreateEntity(u8"slow");
    scene.SetLocalPosition(slowEntity, Float3{-5, 0, -2});
    NavAgentComponent* slow = &scene.GetSystem<NavAgentComponentManager>()->Add(slowEntity);

    // ARRIVE agent: navigateAt with a stop distance parks on the ring, not the point.
    scene::EntityHandle arriveEntity = scene.CreateEntity(u8"arrive");
    scene.SetLocalPosition(arriveEntity, Float3{-5, 0, 2});
    NavAgentComponent* arrive = &scene.GetSystem<NavAgentComponentManager>()->Add(arriveEntity);

    scene.UpdateTransforms();
    scene.Start();
    scene.SetSimulationEnabled(true);
    REQUIRE(slow->agentId >= 0);
    REQUIRE(arrive->agentId >= 0);

    slow->setSpeed(0.8f); // live change - the tick pushes it into the crowd
    slow->navigate(5.0f, 0.0f, -2.0f);
    arrive->navigateAt(5.0f, 0.0f, 2.0f, 3.5f, 2.5f);

    for (int step = 0; step < 90; ++step) // 3 seconds
    {
        scene.Update(1.0f / 30.0f);
    }

    // 3s at 0.8 u/s cannot cross 10 units; the default 3.5 u/s agent with a 2.5 stop ring
    // has already parked.
    const Float3 slowPos = scene.GetWorldPosition(slowEntity);
    CHECK_FALSE(slow->finished);
    CHECK(slowPos.x < 0.0f);        // well short of the target
    CHECK(slowPos.x > -4.5f);       // but moving

    CHECK(arrive->finished);
    const Float3 arrivePos = scene.GetWorldPosition(arriveEntity);
    const f32 dx = arrivePos.x - 5.0f;
    const f32 dz = arrivePos.z - 2.0f;
    const f32 distance = Sqrt(dx * dx + dz * dz);
    CHECK(distance > 1.2f); // parked on the ring, NOT on the point
    CHECK(distance < 3.5f);
    CHECK(arrive->remaining() > 1.2f);

    // Introspection (Lumix parity): the still-moving agent reads as WALKING on a VALID
    // move request with a live speed intent; the parked one has released its target.
    CHECK(slow->state() == 1);       // NavAgentCrowdState::Walking
    CHECK(slow->targetState() == 2); // NavAgentTargetState::Valid
    CHECK(slow->desiredSpeed() > 0.0f);
    CHECK(slow->desiredSpeed() < 1.0f); // capped by the per-call speed
    CHECK(arrive->state() == 1);
    CHECK(arrive->targetState() == 0); // arrival ClearTarget -> None

    // Raising the slow agent's speed mid-run applies live: it now finishes the crossing.
    slow->setSpeed(6.0f);
    for (int step = 0; step < 240 && !slow->finished; ++step)
    {
        scene.Update(1.0f / 30.0f);
    }
    CHECK(slow->finished);

    scene.SetSimulationEnabled(false);
    scene.Stop();
    RemoveTree(u8"scratch_navspeed_db");
}

namespace
{
    struct NavigationWarnings : ILogSink
    {
        Array<String> messages;
        void Write(LogLevel level, StringView category, StringView message) noexcept override
        {
            if (level == LogLevel::Warning && category == u8"Navigation")
            {
                messages.PushBack(String(message));
            }
        }
    };
}

// An agent that cannot join the navmesh never moves; it used to say nothing (a PaperKid car
// placed where the bake had left a hole stood still with no clue why). Outside every zone, or
// with no navmesh where it stands, it now warns; a well-placed agent says nothing.
TEST_CASE("navigation.scene: an agent that cannot join the navmesh says so")
{
    RegisterNavigationResource();
    RegisterNavigationComponentReflection();
    RemoveTree(u8"scratch_navwarn_db");
    foundation::vfs::NativeFileSystem mount(u8"scratch_navwarn_db", foundation::core::DefaultAllocator());
    content::ContentDatabase db(DefaultAllocator(), mount, BinarySerializerFactory(), u8".rasset");
    Array<byte> blob;
    BakeGroundZone(blob);
    auto* zoneInstance = db.RootGroup()->CreateInstance(u8"zone", NavigationZoneSource::StaticType());
    REQUIRE(zoneInstance != nullptr);
    {
        NavigationZoneSource src;
        src.navMeshBlob.Resize(blob.Size());
        MemCopy(src.navMeshBlob.Data(), blob.Data(), blob.Size());
        REQUIRE(zoneInstance->WriteObject(src).IsOk());
    }
    NavigationZoneFactory factory(DefaultAllocator());
    ResourceManager manager(DefaultAllocator(), db);
    manager.AddFactory(&factory);

    scene::Scene scene(DefaultAllocator(), u8"navwarn");
    AddNavigationSceneManagers(scene);
    scene::EntityHandle zoneEntity = scene.CreateEntity(u8"zone");
    NavMeshZoneComponent& zoneComp = scene.GetSystem<NavMeshZoneComponentManager>()->Add(zoneEntity);
    zoneComp.extents = Float3{15, 10, 15};
    zoneComp.zone.SetId(zoneInstance->Id());
    zoneComp.zone.Bind(manager);
    REQUIRE(zoneComp.zone.Get() != nullptr);

    auto* agents = scene.GetSystem<NavAgentComponentManager>();
    const auto place = [&](StringView name, Float3 at)
    {
        scene::EntityHandle e = scene.CreateEntity(name);
        scene.SetLocalPosition(e, at);
        (void)agents->Add(e);
        return e;
    };
    const scene::EntityHandle good = place(u8"walker", Float3{-5, 0, 0});
    const scene::EntityHandle outside = place(u8"stray", Float3{100, 0, 0});
    const scene::EntityHandle floating = place(u8"floater", Float3{0, 5, 0}); // over the mesh, far above it

    NavigationWarnings warnings;
    Logger& logger = GlobalLogger();
    logger.AddSink(&warnings);
    scene.UpdateTransforms();
    scene.Start();
    logger.RemoveSink(&warnings);

    CHECK(agents->Get(good)->agentId >= 0);
    CHECK(agents->Get(outside)->zoneIndex < 0);
    (void)floating; // Detour gives it a slot, in a state that never moves: the warning says so
    REQUIRE(warnings.messages.Size() == 2);
    CHECK(warnings.messages[0].AsView().StartsWith(u8"agent 'stray' at"));
    CHECK(warnings.messages[0].AsView().EndsWith(u8"is in no navigation zone; it will not move"));
    CHECK(warnings.messages[1].AsView().StartsWith(u8"agent 'floater' at"));
    CHECK(warnings.messages[1].AsView().EndsWith(u8"was the zone baked?)"));

    scene.Stop();
    RemoveTree(u8"scratch_navwarn_db");
}

// A zone with no usable navmesh is skipped; it used to be silent, and every agent in it then
// reported standing in no zone, which points the wrong way. The zone itself says so now.
TEST_CASE("navigation.scene: a zone with no usable navmesh says so")
{
    RegisterNavigationComponentReflection();
    scene::Scene scene(DefaultAllocator(), u8"navzone");
    AddNavigationSceneManagers(scene);
    scene::EntityHandle zoneEntity = scene.CreateEntity(u8"Block zone");
    scene.GetSystem<NavMeshZoneComponentManager>()->Add(zoneEntity).extents = Float3{15, 10, 15};

    NavigationWarnings warnings;
    Logger& logger = GlobalLogger();
    logger.AddSink(&warnings);
    scene.UpdateTransforms();
    scene.Start();
    logger.RemoveSink(&warnings);

    REQUIRE(warnings.messages.Size() == 1);
    CHECK(warnings.messages[0].AsView().StartsWith(u8"navigation zone 'Block zone' has no usable navmesh"));
    CHECK(scene.GetSystem<NavMeshZoneComponentManager>()->Get(zoneEntity)->runtimeIndex < 0);
    scene.Stop();
}
