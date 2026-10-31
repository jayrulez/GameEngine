// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// engine.navigation scene integration: a baked zone and a MoveEntity agent. At Start the subsystem
// loads the zone and registers the agent; navigate() then steers it, and the Update tick writes the
// steered position back to the entity transform until it arrives.

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
import foundation.xml;               // XmlDocument (a version 1 zone record)
import foundation.xml.serialization; // XmlSerializer

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

    // Scene: nav managers and subsystem, a zone entity at the origin, an agent at (-5,0,0).
    scene::Scene scene(DefaultAllocator(), u8"nav");
    AddNavigationSceneManagers(scene);

    // The scene system carries the debug-draw settings block (as physics does), default off.
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

    // Steer to the far side and step until arrival (12 s at 30 Hz).
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
// playing scene): the crowd was built over the old navmesh, which the reload parks and later frees.
// The zone must rebuild its crowd over the new one, not keep stepping the old crowd over freed
// memory, and its agents carry on to where they were going.
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
    // The bake and the runtime both use the scale-free RigidPart frame, so a zone on a scaled
    // entity behaves exactly like the unscaled one: the navmesh's world-unit geometry is placed,
    // never warped.
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
        t.scale = Float3{2.0f, 2.0f, 2.0f}; // a scaled zone entity
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
    CHECK(std::abs(end.x - 5.0f) < 1.5f); // same arrival as the unscaled zone: no double scale
    CHECK(std::abs(end.y) < 0.5f);        // ...and on the ground plane, not floating or sunk

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

    // Slow agent: a lower per-call speed covers less ground in the same steps.
    scene::EntityHandle slowEntity = scene.CreateEntity(u8"slow");
    scene.SetLocalPosition(slowEntity, Float3{-5, 0, -2});
    NavAgentComponent* slow = &scene.GetSystem<NavAgentComponentManager>()->Add(slowEntity);

    // Arrive agent: navigateAt with a stop distance parks on the ring, not the point.
    scene::EntityHandle arriveEntity = scene.CreateEntity(u8"arrive");
    scene.SetLocalPosition(arriveEntity, Float3{-5, 0, 2});
    NavAgentComponent* arrive = &scene.GetSystem<NavAgentComponentManager>()->Add(arriveEntity);

    scene.UpdateTransforms();
    scene.Start();
    scene.SetSimulationEnabled(true);
    REQUIRE(slow->agentId >= 0);
    REQUIRE(arrive->agentId >= 0);

    slow->setSpeed(0.8f); // live change: the tick pushes it into the crowd
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
    CHECK(distance > 1.2f); // parked on the ring, not on the point
    CHECK(distance < 3.5f);
    CHECK(arrive->remaining() > 1.2f);

    // Introspection: the still-moving agent reads as walking on a valid move request with a live
    // speed intent; the parked one has released its target.
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

// An agent that cannot join the navmesh never moves, so it must say why: outside every zone, or
// with no navmesh where it stands, it warns; a well-placed agent says nothing.
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

// A zone with no usable navmesh is skipped and says so itself; otherwise every agent in it would
// only report standing in no zone, which points the wrong way.
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

TEST_CASE("navigation.scene: a zone's maxAgents is how many agents its crowd moves")
{
    RegisterNavigationResource();
    RegisterNavigationComponentReflection();
    RemoveTree(u8"scratch_navmax_db");
    foundation::vfs::NativeFileSystem mount(u8"scratch_navmax_db", foundation::core::DefaultAllocator());
    content::ContentDatabase db(DefaultAllocator(), mount, BinarySerializerFactory(), u8".rasset");
    Array<byte> blob;
    BakeGroundZone(blob, 30.0f);
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

    // Joined agents in a zone of `limit` with `count` placed on its mesh.
    auto joined = [&](i32 limit, i32 count)
    {
        scene::Scene scene(DefaultAllocator(), u8"navmax");
        AddNavigationSceneManagers(scene);
        const scene::EntityHandle zoneEntity = scene.CreateEntity(u8"zone");
        NavMeshZoneComponent& zone = scene.GetSystem<NavMeshZoneComponentManager>()->Add(zoneEntity);
        zone.extents = Float3{35, 10, 35};
        zone.maxAgents = limit;
        zone.zone.SetId(zoneInstance->Id());
        zone.zone.Bind(manager);
        auto* agents = scene.GetSystem<NavAgentComponentManager>();
        for (i32 i = 0; i < count; ++i)
        {
            const scene::EntityHandle e = scene.CreateEntity();
            scene.SetLocalPosition(e, Float3{-25.0f + static_cast<f32>(i % 25) * 2.0f, 0, -25.0f + static_cast<f32>(i / 25) * 2.0f});
            NavAgentComponent& agent = agents->Add(e);
            agent.radius = 0.3f;
        }
        scene.UpdateTransforms();
        scene.Start();
        i32 n = 0;
        agents->ForEach([&](NavAgentComponent& a, scene::EntityHandle) { n += a.agentId >= 0 ? 1 : 0; });
        scene.Stop();
        return n;
    };
    CHECK(joined(200, 150) == 150); // a large crowd, past the default 128
    CHECK(joined(3, 5) == 3);       // a small zone takes only what it was given
    RemoveTree(u8"scratch_navmax_db");
}

TEST_CASE("navigation.scene: a version 1 zone record reads with the default agent limit")
{
    RegisterNavigationComponentReflection();
    const TypeInfo& type = TypeOf<NavMeshZoneComponent>();
    CHECK(type.dataVersion == 2u);
    CHECK(type.minReadDataVersion == 1u);
    foundation::xml::XmlDocument doc(DefaultAllocator());
    REQUIRE(doc.Parse(u8"<root><Float3 name=\"extents\"><f32 name=\"x\">20</f32><f32 name=\"y\">10</f32>"
                      u8"<f32 name=\"z\">20</f32></Float3>"
                      u8"<f32 name=\"cellSize\">0.3</f32><f32 name=\"cellHeight\">0.2</f32>"
                      u8"<f32 name=\"agentRadius\">0.6</f32><f32 name=\"agentHeight\">2</f32>"
                      u8"<f32 name=\"agentMaxClimb\">0.9</f32><f32 name=\"agentMaxSlopeDegrees\">45</f32>"
                      u8"<string name=\"zone\">00000000-0000-0000-0000-000000000000</string></root>") ==
            foundation::xml::XmlResult::Ok);
    foundation::xml::XmlSerializer reader(doc);
    const SerializedDataVersion chain[] = {{type.id, 1}};
    reader.PushVersionScope(chain, 1);
    NavMeshZoneComponent zone;
    zone.maxAgents = 7;
    Serialize(reader, zone);
    reader.PopVersionScope();
    CHECK(reader.IsOk()); // no maxAgents asked of a version 1 record
    CHECK(zone.maxAgents == 7); // left as it was: the component's default (128) for a fresh one
    CHECK(zone.agentRadius == doctest::Approx(0.6f));
}

// A warp puts the agent somewhere else at once (a respawn): moving its entity alone is undone, as
// the crowd owns the position. Warped mid-walk, it stops where it lands, and walks on from there.
TEST_CASE("navigation.scene: a warped agent is where it was sent, and walks on from there")
{
    RegisterNavigationResource();
    RegisterNavigationComponentReflection();
    RemoveTree(u8"scratch_navwarp_db");
    foundation::vfs::NativeFileSystem mount(u8"scratch_navwarp_db", foundation::core::DefaultAllocator());
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

    scene::Scene scene(DefaultAllocator(), u8"nav");
    AddNavigationSceneManagers(scene);
    scene::EntityHandle zoneEntity = scene.CreateEntity(u8"zone");
    NavMeshZoneComponent& zoneComp = scene.GetSystem<NavMeshZoneComponentManager>()->Add(zoneEntity);
    zoneComp.extents = Float3{15, 10, 15};
    zoneComp.maxAgents = 1; // one slot: a warp that kept its old one could not take a new one
    zoneComp.zone.SetId(zoneInstance->Id());
    zoneComp.zone.Bind(manager);
    scene::EntityHandle agentEntity = scene.CreateEntity(u8"agent");
    scene.SetLocalPosition(agentEntity, Float3{-5, 0, 0});
    NavAgentComponent* agent = &scene.GetSystem<NavAgentComponentManager>()->Add(agentEntity);
    scene.UpdateTransforms();
    scene.Start();
    scene.SetSimulationEnabled(true);
    REQUIRE(agent->agentId >= 0);

    // Walking east, it is warped to the far corner: there next tick, stopped.
    agent->navigate(5.0f, 0.0f, 0.0f);
    for (int step = 0; step < 10; ++step)
    {
        scene.Update(1.0f / 30.0f);
    }
    agent->warp(6.0f, 0.0f, 6.0f);
    scene.Update(1.0f / 30.0f);
    CHECK(agent->agentId >= 0); // back in its crowd
    Float3 at = scene.GetWorldPosition(agentEntity);
    CHECK(std::abs(at.x - 6.0f) < 0.3f);
    CHECK(std::abs(at.z - 6.0f) < 0.3f);
    CHECK(agent->finished);
    // And it stays there: the crowd holds it where it was sent, not where it was.
    for (int step = 0; step < 15; ++step)
    {
        scene.Update(1.0f / 30.0f);
    }
    at = scene.GetWorldPosition(agentEntity);
    CHECK(std::abs(at.x - 6.0f) < 0.3f);
    CHECK(std::abs(at.z - 6.0f) < 0.3f);

    // From there it walks on to a new target.
    agent->navigate(6.0f, 0.0f, -6.0f);
    for (int step = 0; step < 360 && !agent->finished; ++step)
    {
        scene.Update(1.0f / 30.0f);
    }
    at = scene.GetWorldPosition(agentEntity);
    CHECK(agent->finished);
    CHECK(std::abs(at.z + 6.0f) < 1.5f);

    scene.SetSimulationEnabled(false);
    scene.Stop();
    RemoveTree(u8"scratch_navwarp_db");
}

// move(vx, vz): an agent driven by a velocity (a player's stick) instead of to a point. No path is
// planned, so it starts the frame it is asked, keeps the speed asked, and stands on move(0, 0).
TEST_CASE("navigation.scene: an agent moved by a velocity goes at once, at that speed")
{
    RegisterNavigationResource();
    RegisterNavigationComponentReflection();
    RemoveTree(u8"scratch_navmove_db");
    foundation::vfs::NativeFileSystem mount(u8"scratch_navmove_db", foundation::core::DefaultAllocator());
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

    scene::Scene scene(DefaultAllocator(), u8"nav");
    AddNavigationSceneManagers(scene);
    scene::EntityHandle zoneEntity = scene.CreateEntity(u8"zone");
    NavMeshZoneComponent& zoneComp = scene.GetSystem<NavMeshZoneComponentManager>()->Add(zoneEntity);
    zoneComp.extents = Float3{15, 10, 15};
    zoneComp.zone.SetId(zoneInstance->Id());
    zoneComp.zone.Bind(manager);
    scene::EntityHandle agentEntity = scene.CreateEntity(u8"agent");
    scene.SetLocalPosition(agentEntity, Float3{-5, 0, 0});
    NavAgentComponent* agent = &scene.GetSystem<NavAgentComponentManager>()->Add(agentEntity);
    agent->maxSpeed = 3.0f;
    scene.UpdateTransforms();
    scene.Start();
    scene.SetSimulationEnabled(true);
    REQUIRE(agent->agentId >= 0);

    // East at 1.5 m/s: moving within a few frames, then 1.5 m a second.
    const f32 dt = 1.0f / 30.0f;
    for (int step = 0; step < 3; ++step)
    {
        agent->move(1.5f, 0.0f);
        scene.Update(dt);
    }
    CHECK(scene.GetWorldPosition(agentEntity).x > -5.0f + 0.01f);
    const f32 before = scene.GetWorldPosition(agentEntity).x;
    for (int step = 0; step < 60; ++step)
    {
        agent->move(1.5f, 0.0f);
        scene.Update(dt);
    }
    const f32 travelled = scene.GetWorldPosition(agentEntity).x - before;
    CHECK(travelled == doctest::Approx(3.0f).epsilon(0.1)); // 2 s at 1.5 m/s
    CHECK(std::abs(scene.GetWorldPosition(agentEntity).z) < 0.1f);

    // move(0, 0): it stands where it is, still in the crowd.
    for (int step = 0; step < 30; ++step)
    {
        agent->move(0.0f, 0.0f);
        scene.Update(dt);
    }
    const f32 stood = scene.GetWorldPosition(agentEntity).x;
    for (int step = 0; step < 15; ++step)
    {
        scene.Update(dt);
    }
    CHECK(std::abs(scene.GetWorldPosition(agentEntity).x - stood) < 0.02f);

    scene.SetSimulationEnabled(false);
    scene.Stop();
    RemoveTree(u8"scratch_navmove_db");
}

TEST_CASE("navigation.scene: a version 2 agent record reads as one that avoids others")
{
    RegisterNavigationComponentReflection();
    const TypeInfo& type = TypeOf<NavAgentComponent>();
    CHECK(type.dataVersion == 3u);
    CHECK(type.minReadDataVersion == 2u);
    foundation::xml::XmlDocument doc(DefaultAllocator());
    REQUIRE(doc.Parse(u8"<root><f32 name=\"radius\">0.3</f32><f32 name=\"height\">1.7</f32>"
                      u8"<f32 name=\"maxSpeed\">3</f32><f32 name=\"maxAcceleration\">6</f32>"
                      u8"<bool name=\"moveEntity\">true</bool><f32 name=\"stopDistance\">0</f32></root>") ==
            foundation::xml::XmlResult::Ok);
    foundation::xml::XmlSerializer reader(doc);
    const SerializedDataVersion chain[] = {{type.id, 2}};
    reader.PushVersionScope(chain, 1);
    NavAgentComponent agent;
    Serialize(reader, agent);
    reader.PopVersionScope();
    CHECK(reader.IsOk()); // no avoidOthers asked of a version 2 record
    CHECK(agent.avoidOthers);
    CHECK(agent.radius == doctest::Approx(0.3f));
}

// avoidOthers: a velocity-driven agent that avoids others holds back from one standing in its way
// (Detour's avoidance will not close on it: to a player, an invisible wall); one that does not
// shoulders past, the crowd's collision step pushing the two apart as it goes.
TEST_CASE("navigation.scene: an agent that does not avoid others shoulders past one in its way")
{
    RegisterNavigationResource();
    RegisterNavigationComponentReflection();
    RemoveTree(u8"scratch_navavoid_db");
    foundation::vfs::NativeFileSystem mount(u8"scratch_navavoid_db", foundation::core::DefaultAllocator());
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

    // How far off its line (|z|) a walker driven east past a standing agent strays, the least gap
    // between them, and where it ends up.
    struct Run
    {
        f32 stray = 0.0f;
        f32 closest = 100.0f;
        f32 endX = 0.0f;
    };
    auto walk = [&](bool avoidOthers, bool offOnceRunning = false) -> Run
    {
        scene::Scene scene(DefaultAllocator(), u8"nav");
        AddNavigationSceneManagers(scene);
        scene::EntityHandle zoneEntity = scene.CreateEntity(u8"zone");
        NavMeshZoneComponent& zoneComp = scene.GetSystem<NavMeshZoneComponentManager>()->Add(zoneEntity);
        zoneComp.extents = Float3{15, 10, 15};
        zoneComp.zone.SetId(zoneInstance->Id());
        zoneComp.zone.Bind(manager);
        auto* agents = scene.GetSystem<NavAgentComponentManager>();
        scene::EntityHandle standing = scene.CreateEntity(u8"standing");
        scene.SetLocalPosition(standing, Float3{0, 0, 0.15f}); // a little off the line, as people are
        agents->Add(standing).radius = 0.4f;
        scene::EntityHandle walker = scene.CreateEntity(u8"walker");
        scene.SetLocalPosition(walker, Float3{-4, 0, 0});
        NavAgentComponent* w = &agents->Add(walker);
        w->radius = 0.4f;
        w->avoidOthers = avoidOthers;
        scene.UpdateTransforms();
        scene.Start();
        scene.SetSimulationEnabled(true);
        Run run;
        for (int step = 0; step < 150; ++step) // 5 s at 30 Hz, 1.5 m/s
        {
            w = agents->Get(walker);
            if (offOnceRunning && step == 1)
            {
                w->avoidOthers = false; // changed live, as a script or the inspector does
            }
            w->move(1.5f, 0.0f);
            scene.Update(1.0f / 30.0f);
            const Float3 p = scene.GetWorldPosition(walker);
            const Float3 q = scene.GetWorldPosition(standing);
            run.stray = Max(run.stray, std::abs(p.z));
            run.closest = Min(run.closest, std::sqrt((p.x - q.x) * (p.x - q.x) + (p.z - q.z) * (p.z - q.z)));
        }
        run.endX = scene.GetWorldPosition(walker).x;
        scene.SetSimulationEnabled(false);
        scene.Stop();
        return run;
    };

    const Run avoiding = walk(true);
    const Run straight = walk(false);
    const Run turnedOff = walk(true, true);
    CHECK(turnedOff.endX > 1.0f);     // the change reached its crowd: it went past too
    CHECK(avoiding.closest > 1.0f);   // held back, short of the one in its way
    CHECK(avoiding.endX < 0.0f);      // and never got past it
    CHECK(straight.endX > 1.0f);      // past it
    CHECK(straight.closest > 0.3f);   // brushing it aside (radii 0.4 + 0.4), not through its middle
    CHECK(straight.stray < 0.5f);     // near its line
    RemoveTree(u8"scratch_navavoid_db");
}

// An agent added after the scene starts (a spawned prefab's, a network spawn's) joins the crowd on
// the next update, and a destroyed one leaves it: in a zone that takes one agent, the first, gone,
// makes room for a second added later, which then walks.
TEST_CASE("navigation.scene: an agent added later joins its crowd, and a destroyed one leaves it")
{
    RegisterNavigationResource();
    RegisterNavigationComponentReflection();
    RemoveTree(u8"scratch_navlate_db");
    foundation::vfs::NativeFileSystem mount(u8"scratch_navlate_db", foundation::core::DefaultAllocator());
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

    scene::Scene scene(DefaultAllocator(), u8"navlate");
    AddNavigationSceneManagers(scene);
    const scene::EntityHandle zoneEntity = scene.CreateEntity(u8"zone");
    NavMeshZoneComponent& zone = scene.GetSystem<NavMeshZoneComponentManager>()->Add(zoneEntity);
    zone.extents = Float3{15, 10, 15};
    zone.maxAgents = 1; // room for one
    zone.zone.SetId(zoneInstance->Id());
    zone.zone.Bind(manager);
    auto* agents = scene.GetSystem<NavAgentComponentManager>();
    const scene::EntityHandle first = scene.CreateEntity(u8"first");
    scene.SetLocalPosition(first, Float3{-5, 0, 0});
    agents->Add(first);
    scene.UpdateTransforms();
    scene.Start();
    scene.SetSimulationEnabled(true);
    REQUIRE(agents->Get(first)->agentId >= 0);

    scene.DestroyEntity(first);
    scene.Update(1.0f / 30.0f); // its slot is given back

    const scene::EntityHandle second = scene.CreateEntity(u8"second");
    scene.SetLocalPosition(second, Float3{-5, 0, 2});
    agents->Add(second);
    scene.UpdateTransforms();
    scene.Update(1.0f / 30.0f); // it joins
    NavAgentComponent* agent = agents->Get(second);
    REQUIRE(agent != nullptr);
    CHECK(agent->agentId >= 0);

    agent->navigate(5.0f, 0.0f, 2.0f);
    for (int step = 0; step < 360 && !agents->Get(second)->finished; ++step)
    {
        scene.Update(1.0f / 30.0f);
    }
    CHECK(agents->Get(second)->finished);
    CHECK(std::abs(scene.GetWorldPosition(second).x - 5.0f) < 1.5f);

    scene.SetSimulationEnabled(false);
    scene.Stop();
    RemoveTree(u8"scratch_navlate_db");
}

// A destroyed agent's crowd slot is freed in the crowd it was in, before a reloaded navmesh
// rebuilds that crowd: the rebuilt crowd numbers its agents afresh, so the old slot number would
// name another agent there. Here the first is destroyed in the frame the zone reloads, and the
// second, which the rebuilt crowd puts in slot 0, still walks to where it was going.
TEST_CASE("navigation.scene: an agent destroyed as its zone reloads leaves the old crowd, not another agent")
{
    RegisterNavigationResource();
    RegisterNavigationComponentReflection();
    RemoveTree(u8"scratch_navgonereload_db");
    foundation::vfs::NativeFileSystem mount(u8"scratch_navgonereload_db", foundation::core::DefaultAllocator());
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

    scene::Scene scene(DefaultAllocator(), u8"navgonereload");
    AddNavigationSceneManagers(scene);
    const scene::EntityHandle zoneEntity = scene.CreateEntity(u8"zone");
    NavMeshZoneComponent& zone = scene.GetSystem<NavMeshZoneComponentManager>()->Add(zoneEntity);
    zone.extents = Float3{15, 10, 15};
    zone.zone.SetId(zoneInstance->Id());
    zone.zone.Bind(manager);
    auto* agents = scene.GetSystem<NavAgentComponentManager>();
    const scene::EntityHandle first = scene.CreateEntity(u8"first");
    scene.SetLocalPosition(first, Float3{-5, 0, 3});
    agents->Add(first);
    const scene::EntityHandle second = scene.CreateEntity(u8"second");
    scene.SetLocalPosition(second, Float3{-5, 0, -3});
    agents->Add(second);
    scene.UpdateTransforms();
    scene.Start();
    scene.SetSimulationEnabled(true);
    REQUIRE(agents->Get(first)->agentId == 0);
    REQUIRE(agents->Get(second)->agentId == 1);

    agents->Get(second)->navigate(5.0f, 0.0f, -3.0f);
    for (int step = 0; step < 10; ++step)
    {
        scene.Update(1.0f / 30.0f);
    }

    // In one frame: the first goes and the zone's navmesh reloads.
    scene.DestroyEntity(first);
    write(*zoneInstance, 12.0f);
    REQUIRE(manager.Reload(zoneInstance->Id()));
    scene.Update(1.0f / 30.0f);
    REQUIRE(agents->Get(second) != nullptr);
    CHECK(agents->Get(second)->agentId == 0); // the rebuilt crowd's first slot

    for (int step = 0; step < 360 && !agents->Get(second)->finished; ++step)
    {
        scene.Update(1.0f / 30.0f);
    }
    CHECK(agents->Get(second)->finished);
    CHECK(std::abs(scene.GetWorldPosition(second).x - 5.0f) < 1.5f);

    scene.SetSimulationEnabled(false);
    scene.Stop();
    RemoveTree(u8"scratch_navgonereload_db");
}
