// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor navigation bake: a scene with a static ground body and a zone; collect the in-zone static
// geometry in zone-local space, run the Recast bake and write the NavigationZoneAsset sidecar. The
// written blob loads back into a NavigationMesh and paths, covering the whole author-side chain end
// to end (headless).

#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.vfs;
import foundation.content;
import foundation.resource;
import foundation.scene;
import foundation.geometry;
import engine.render;
import engine.physics;
import foundation.physics;
import engine.navigation;
import engine.terrain;
import foundation.terrain.resource;
import foundation.heightfield;
import foundation.navigation;
import foundation.navigation.resource;
import navigation.pipeline;
import editor.navigation;

using namespace foundation::core;
namespace scene = foundation::scene;
namespace geometry = foundation::geometry;
namespace content = foundation::content;
using namespace foundation::navigation;

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

    // A wall-like render mesh across the zone (x 0, z -15..15, 3 m high), both faces: it blocks
    // nothing, since render meshes are not level geometry.
    RefPtr<geometry::StaticMesh> WallMesh()
    {
        RefPtr<geometry::StaticMesh> mesh = MakeRef<geometry::StaticMesh>(DefaultAllocator());
        const Float3 corners[] = {{0, 0, -15}, {0, 3, -15}, {0, 3, 15}, {0, 0, 15}};
        for (const Float3& p : corners)
        {
            geometry::StaticMeshVertex v;
            v.position = p;
            v.normal = Float3{1, 0, 0};
            mesh->vertices.PushBack(v);
        }
        const u32 tris[] = {0, 1, 2, 0, 2, 3, 0, 2, 1, 0, 3, 2};
        mesh->indices.Resize(sizeof(tris) / sizeof(tris[0])); // Add writes into a sized buffer
        for (u32 i : tris)
        {
            mesh->indices.Add(i);
        }
        mesh->CalculateBounds();
        return mesh;
    }

    // Collect the zone's static geometry and bake it with the zone's parameters (empty = nothing
    // to bake, or Recast found no walkable surface).
    Array<byte> BakeCollected(scene::Scene& sceneObj, scene::EntityHandle zoneEntity, usize& outTriangles)
    {
        const engine::navigation::NavMeshZoneComponent& z =
            *sceneObj.GetSystem<engine::navigation::NavMeshZoneComponentManager>()->Get(zoneEntity);
        Array<Float3> verts;
        Array<u32> indices;
        outTriangles = editor::navigation::CollectNavigationGeometry(
            sceneObj, zoneEntity, z.extents, z.cellSize, verts, indices);

        Array<byte> blob;
        if (outTriangles > 0)
        {
            NavigationBakeParams params;
            params.cellSize = z.cellSize;
            params.cellHeight = z.cellHeight;
            params.agentRadius = z.agentRadius;
            params.agentHeight = z.agentHeight;
            params.agentMaxClimb = z.agentMaxClimb;
            params.agentMaxSlopeDegrees = z.agentMaxSlopeDegrees;
            (void)NavigationMeshBuilder::Build(Span<const Float3>{verts.Data(), verts.Size()},
                                               Span<const u32>{indices.Data(), indices.Size()},
                                               params, blob);
        }
        return blob;
    }

    // A box rigid body: what the bake reads when it is static and solid.
    scene::EntityHandle AddBody(scene::Scene& sceneObj, StringView name, Float3 position, Float3 half,
                                foundation::physics::MotionKind motion, bool trigger = false)
    {
        scene::EntityHandle e = sceneObj.CreateEntity(name);
        sceneObj.SetLocalPosition(e, position);
        engine::physics::RigidBodyComponent& body =
            sceneObj.GetSystem<engine::physics::RigidBodyComponentManager>()->Add(e);
        body.motion = motion;
        body.halfExtents = half;
        body.isTrigger = trigger;
        return e;
    }

    // An in-memory terrain over a 65x65 heightfield (the smallest legal grid): heights come
    // from `heightAt(gx, gz)` in world Y, quantized onto [minY, maxY].
    RefPtr<foundation::terrain::TerrainResource> MakeTerrain(f32 minY, f32 maxY, f32 worldSide,
                                                             f32 (*heightAt)(i32, i32))
    {
        constexpr i32 kSide = 65;
        RefPtr<foundation::heightfield::Heightfield> field =
            MakeRef<foundation::heightfield::Heightfield>(
                DefaultAllocator(), kSide, Float2{worldSide, worldSide}, minY, maxY);
        for (i32 gz = 0; gz < kSide; ++gz)
        {
            for (i32 gx = 0; gx < kSide; ++gx)
            {
                field->SetSample(gx, gz, field->WorldYToSample(heightAt(gx, gz)));
            }
        }
        RefPtr<foundation::terrain::TerrainResource> terrain =
            MakeRef<foundation::terrain::TerrainResource>(DefaultAllocator());
        terrain->heightfield = field;
        return terrain;
    }

    // Scene, zone and one terrain entity; returns the baked navmesh blob (empty when the bake
    // failed).
    Array<byte> BakeTerrainZone(RefPtr<foundation::terrain::TerrainResource> terrain,
                                Float3 zoneExtents, usize& outTriangles)
    {
        scene::Scene sceneObj(DefaultAllocator(), u8"bake_terrain");
        engine::navigation::AddNavigationSceneManagers(sceneObj);
        auto* terrains = sceneObj.AddSystem<engine::terrain::TerrainComponentManager>();

        scene::EntityHandle terrainEntity = sceneObj.CreateEntity(u8"terrain");
        engine::terrain::TerrainComponent& tc = terrains->Add(terrainEntity);
        tc.terrain = foundation::resource::Ref<foundation::terrain::TerrainResource>(terrain);

        scene::EntityHandle zoneEntity = sceneObj.CreateEntity(u8"zone");
        engine::navigation::NavMeshZoneComponent& z =
            sceneObj.GetSystem<engine::navigation::NavMeshZoneComponentManager>()->Add(zoneEntity);
        z.extents = zoneExtents;
        sceneObj.UpdateTransforms();

        return BakeCollected(sceneObj, zoneEntity, outTriangles);
    }

    bool PathAcross(Span<const byte> blob, Float3 from, Float3 to)
    {
        NavigationMesh mesh(DefaultAllocator());
        if (!mesh.Load(blob).IsOk() || !mesh.IsValid())
        {
            return false;
        }
        NavigationMeshQuery query(DefaultAllocator(), mesh);
        NavigationPath path;
        return query.FindPath(from, to, path).IsOk() && path.complete;
    }
}

TEST_CASE("editor.navigation: bake collects scene geometry and writes a loadable zone asset")
{
    RegisterNavigationResource();
    pipeline::RegisterNavigationZoneAsset();
    RemoveTree(u8"scratch_navbake_db");

    foundation::vfs::NativeFileSystem mount(u8"scratch_navbake_db", foundation::core::DefaultAllocator());
    content::ContentDatabase db(DefaultAllocator(), mount, BinarySerializerFactory(), u8".rasset");
    auto* assetInstance =
        db.RootGroup()->CreateInstance(u8"zone", pipeline::NavigationZoneAsset::StaticType());
    REQUIRE(assetInstance != nullptr);

    // Scene: a static ground slab at the origin (its top at y 0) and a zone entity that covers it.
    scene::Scene sceneObj(DefaultAllocator(), u8"bake");
    engine::navigation::AddNavigationSceneManagers(sceneObj);
    sceneObj.AddSystem<engine::physics::RigidBodyComponentManager>();
    (void)AddBody(sceneObj, u8"ground", Float3{0, -0.5f, 0}, Float3{10, 0.5f, 10},
                  foundation::physics::MotionKind::Static);

    scene::EntityHandle zoneEntity = sceneObj.CreateEntity(u8"zone");
    engine::navigation::NavMeshZoneComponent& z =
        sceneObj.GetSystem<engine::navigation::NavMeshZoneComponentManager>()->Add(zoneEntity);
    z.extents = Float3{15, 10, 15};

    sceneObj.UpdateTransforms();

    // Bake: collect the slab's triangles (a box: 12) and write the navmesh into the asset sidecar.
    const editor::navigation::BakeResult result =
        editor::navigation::BakeNavigationZone(sceneObj, zoneEntity, *assetInstance);
    CHECK(result.triangleCount == 12u);
    CHECK(result.baked);
    // What the editor tells the author: the asset is written, so a cook applies it; the scene was
    // not touched, so there is nothing to save.
    const String told = editor::navigation::DescribeBake(result);
    CHECK(told.AsView().ContainsIgnoreCase(u8"cook"));
    CHECK_FALSE(told.AsView().ContainsIgnoreCase(u8"save"));

    // The written asset carries a navmesh (in the sidecar) that loads and paths.
    pipeline::NavigationZoneAsset readBack;
    {
        RefPtr<ISerializable> object = assetInstance->ReadObject();
        auto* asset = Cast<pipeline::NavigationZoneAsset>(object.Get());
        REQUIRE(asset != nullptr);
        REQUIRE(pipeline::EnsureNavMeshLoaded(*assetInstance, *asset).IsOk());
        REQUIRE(!asset->navMeshBlob.IsEmpty());
        readBack.navMeshBlob = asset->navMeshBlob;
    }
    NavigationMesh mesh(DefaultAllocator());
    REQUIRE(mesh.Load(Span<const byte>{reinterpret_cast<const byte*>(readBack.navMeshBlob.Data()),
                                       readBack.navMeshBlob.Size()})
                .IsOk());
    REQUIRE(mesh.IsValid());
    NavigationMeshQuery query(DefaultAllocator(), mesh);
    NavigationPath path;
    REQUIRE(query.FindPath(Float3{-8, 0, 0}, Float3{8, 0, 0}, path).IsOk());
    CHECK(path.complete);

    RemoveTree(u8"scratch_navbake_db");
}

TEST_CASE("editor.navigation: bake succeeds when the zone shares a SCALED entity with its ground")
{
    // A nav zone authored directly on the ground entity, where that entity is scaled up (for
    // example a unit plane scaled to 20x20). The bake frame must be rigid: otherwise
    // Inverse(zoneWorld) cancels the entity's scale, hands Recast a 1x1 plane, and the agent-radius
    // erosion wipes it ("No walkable geometry inside the zone"). With RigidPart the geometry keeps
    // its true world size and bakes.
    pipeline::RegisterNavigationZoneAsset();
    RemoveTree(u8"scratch_navbake_scaled_db");

    foundation::vfs::NativeFileSystem mount(u8"scratch_navbake_scaled_db", foundation::core::DefaultAllocator());
    content::ContentDatabase db(DefaultAllocator(), mount, BinarySerializerFactory(), u8".rasset");
    auto* assetInstance =
        db.RootGroup()->CreateInstance(u8"zone", pipeline::NavigationZoneAsset::StaticType());
    REQUIRE(assetInstance != nullptr);

    scene::Scene sceneObj(DefaultAllocator(), u8"bake_scaled");
    engine::navigation::AddNavigationSceneManagers(sceneObj);
    sceneObj.AddSystem<engine::physics::RigidBodyComponentManager>();

    // One entity carries both the ground slab and the zone, scaled up 20x in X and Z (a primitive
    // body keeps its world size, as physics builds it; the frame must not shrink it).
    scene::EntityHandle entity = AddBody(sceneObj, u8"ground_zone", Float3{0, 0, 0},
                                         Float3{10, 0.5f, 10}, foundation::physics::MotionKind::Static);

    engine::navigation::NavMeshZoneComponent& z =
        sceneObj.GetSystem<engine::navigation::NavMeshZoneComponentManager>()->Add(entity);
    z.extents = Float3{15, 10, 15};

    Transform t = sceneObj.GetLocalTransform(entity);
    t.scale = Float3{20, 1, 20};
    sceneObj.SetLocalTransform(entity, t);
    sceneObj.UpdateTransforms();

    const editor::navigation::BakeResult result =
        editor::navigation::BakeNavigationZone(sceneObj, entity, *assetInstance);
    CHECK(result.triangleCount == 12u); // the slab's box
    CHECK(result.baked);                // fails without RigidPart: the geometry shrinks 20x and erodes away

    RemoveTree(u8"scratch_navbake_scaled_db");
}

TEST_CASE("editor.navigation: terrain contributes walkable surface to the bake")
{
    usize triangles = 0;
    Array<byte> blob = BakeTerrainZone(
        MakeTerrain(0.0f, 10.0f, 32.0f, [](i32, i32) { return 2.0f; }), Float3{14, 8, 14},
        triangles);
    CHECK(triangles > 0);
    REQUIRE(!blob.IsEmpty());
    CHECK(PathAcross(Span<const byte>(blob.Data(), blob.Size()), Float3{-10, 2, 0},
                     Float3{10, 2, 0}));
}

TEST_CASE("editor.navigation: a terrain hole is no walkable surface - a strip across the zone blocks the path")
{
    // The terrain holes rule: a stride block with a cut sample is left out of the bake, so the
    // navmesh opens there. A strip of cut samples across the whole zone leaves the agent no way
    // from one side to the other.
    RefPtr<foundation::terrain::TerrainResource> terrain =
        MakeTerrain(0.0f, 10.0f, 32.0f, [](i32, i32) { return 2.0f; });
    usize solidTriangles = 0;
    Array<byte> solid = BakeTerrainZone(terrain, Float3{14, 8, 14}, solidTriangles);
    REQUIRE(!solid.IsEmpty());
    REQUIRE(PathAcross(Span<const byte>(solid.Data(), solid.Size()), Float3{-10, 2, 0},
                       Float3{10, 2, 0}));

    foundation::heightfield::Heightfield* field = terrain->heightfield.Get();
    REQUIRE(field != nullptr);
    for (i32 gz = 0; gz < field->Size(); ++gz) // the strip: samples 30..34 (x ~ -1..1 m) at every z
    {
        for (i32 gx = 30; gx <= 34; ++gx)
        {
            field->SetHole(gx, gz, true);
        }
    }
    usize holedTriangles = 0;
    Array<byte> holed = BakeTerrainZone(terrain, Float3{14, 8, 14}, holedTriangles);
    CHECK(holedTriangles < solidTriangles); // the strip's blocks are gone
    REQUIRE(!holed.IsEmpty());
    CHECK_FALSE(PathAcross(Span<const byte>(holed.Data(), holed.Size()), Float3{-10, 2, 0},
                           Float3{10, 2, 0}));
    // Both halves stay walkable on their own side.
    CHECK(PathAcross(Span<const byte>(holed.Data(), holed.Size()), Float3{-10, 2, -5},
                     Float3{-10, 2, 5}));
}

TEST_CASE("editor.navigation: an agent paths across sloped terrain")
{
    // A gentle ramp: ~4 units of rise over the 32-unit footprint (~7 degrees).
    usize triangles = 0;
    Array<byte> blob = BakeTerrainZone(
        MakeTerrain(0.0f, 10.0f, 32.0f,
                    [](i32 gx, i32) { return static_cast<f32>(gx) * (4.0f / 64.0f); }),
        Float3{14, 8, 14}, triangles);
    REQUIRE(!blob.IsEmpty());
    CHECK(PathAcross(Span<const byte>(blob.Data(), blob.Size()), Float3{-10, 1, 0},
                     Float3{10, 3, 0}));
}

TEST_CASE("editor.navigation: a too-steep terrain wall splits the navmesh")
{
    // A cliff across the middle: ~24 units of rise over one cell (~88 degrees), far past the
    // walkable-slope filter, so the two plateaus must not connect.
    usize triangles = 0;
    Array<byte> blob = BakeTerrainZone(
        MakeTerrain(0.0f, 30.0f, 32.0f,
                    [](i32 gx, i32) { return gx < 32 ? 1.0f : 25.0f; }),
        Float3{14, 28, 14}, triangles);
    REQUIRE(!blob.IsEmpty());
    CHECK(!PathAcross(Span<const byte>(blob.Data(), blob.Size()), Float3{-10, 1, 0},
                      Float3{10, 25, 0}));
    // Each plateau itself remains walkable.
    CHECK(PathAcross(Span<const byte>(blob.Data(), blob.Size()), Float3{-10, 1, 0},
                     Float3{-3, 1, 6}));
}

// Reading render meshes would let a car or a walker standing in a zone bake a hole in the road
// under itself, so only static, solid bodies are level geometry: dynamic and kinematic bodies,
// triggers and bare render meshes across the floor leave the path open, and a static wall in the
// same place cuts it.
TEST_CASE("editor.navigation: only static, solid bodies are baked; what moves and bare meshes are not")
{
    const auto bake = [](bool staticWall, usize& triangles)
    {
        scene::Scene sceneObj(DefaultAllocator(), u8"bake_movers");
        engine::navigation::AddNavigationSceneManagers(sceneObj);
        sceneObj.AddSystem<engine::physics::RigidBodyComponentManager>();
        auto* meshes = sceneObj.AddSystem<engine::render::MeshComponentManager>();
        using foundation::physics::MotionKind;
        (void)AddBody(sceneObj, u8"ground", Float3{0, -0.5f, 0}, Float3{10, 0.5f, 10}, MotionKind::Static);
        // Across the middle, x 0: a car, a lift, a trigger, and a wall that is only a mesh.
        (void)AddBody(sceneObj, u8"car", Float3{0, 1, -5}, Float3{1, 1, 2}, MotionKind::Dynamic);
        (void)AddBody(sceneObj, u8"lift", Float3{0, 1, 0}, Float3{1, 1, 2}, MotionKind::Kinematic);
        (void)AddBody(sceneObj, u8"porch", Float3{0, 1, 5}, Float3{1, 1, 2}, MotionKind::Static, true);
        RefPtr<geometry::StaticMesh> wall = WallMesh();
        meshes->Add(sceneObj.CreateEntity(u8"mesh wall")).mesh = foundation::resource::Ref<geometry::StaticMesh>(wall);
        if (staticWall)
        {
            (void)AddBody(sceneObj, u8"wall", Float3{0, 1.5f, 0}, Float3{0.5f, 1.5f, 15}, MotionKind::Static);
        }
        scene::EntityHandle zoneEntity = sceneObj.CreateEntity(u8"zone");
        sceneObj.GetSystem<engine::navigation::NavMeshZoneComponentManager>()->Add(zoneEntity).extents =
            Float3{15, 10, 15};
        sceneObj.UpdateTransforms();
        return BakeCollected(sceneObj, zoneEntity, triangles);
    };

    usize triangles = 0;
    Array<byte> open = bake(false, triangles);
    CHECK(triangles == 12u); // the ground's box, nothing else
    REQUIRE(!open.IsEmpty());
    CHECK(PathAcross(Span<const byte>{open.Data(), open.Size()}, Float3{-8, 0, 0}, Float3{8, 0, 0}));
    CHECK(PathAcross(Span<const byte>{open.Data(), open.Size()}, Float3{-8, 0, -5}, Float3{8, 0, -5}));

    Array<byte> walled = bake(true, triangles);
    CHECK(triangles == 24u); // the ground and the wall
    REQUIRE(!walled.IsEmpty());
    CHECK_FALSE(PathAcross(Span<const byte>{walled.Data(), walled.Size()}, Float3{-8, 0, 0}, Float3{8, 0, 0}));
}

// A level's ground is often a static plane as wide as physics allows (2000 m, for example). Its
// triangles reach far past the zone, and sizing the navmesh grid from them gives a 2000 m grid
// that does not load at runtime, so the zone's box bounds the bake.
TEST_CASE("editor.navigation: a ground plane wider than the zone bakes to the zone's box")
{
    pipeline::RegisterNavigationZoneAsset();
    RemoveTree(u8"scratch_navbake_plane_db");
    foundation::vfs::NativeFileSystem mount(u8"scratch_navbake_plane_db", foundation::core::DefaultAllocator());
    content::ContentDatabase db(DefaultAllocator(), mount, BinarySerializerFactory(), u8".rasset");
    auto* assetInstance = db.RootGroup()->CreateInstance(u8"zone", pipeline::NavigationZoneAsset::StaticType());
    REQUIRE(assetInstance != nullptr);

    scene::Scene sceneObj(DefaultAllocator(), u8"bake_plane");
    engine::navigation::AddNavigationSceneManagers(sceneObj);
    sceneObj.AddSystem<engine::physics::RigidBodyComponentManager>();
    const scene::EntityHandle ground = AddBody(sceneObj, u8"ground", Float3{0, 0, 0}, Float3{1, 1, 1},
                                               foundation::physics::MotionKind::Static);
    engine::physics::RigidBodyComponent& plane =
        *sceneObj.GetSystem<engine::physics::RigidBodyComponentManager>()->Get(ground);
    plane.shape = foundation::physics::ShapeKind::Plane;
    plane.planeHalfExtent = 1000.0f;
    scene::EntityHandle zoneEntity = sceneObj.CreateEntity(u8"zone");
    sceneObj.GetSystem<engine::navigation::NavMeshZoneComponentManager>()->Add(zoneEntity).extents =
        Float3{20, 6, 20};
    sceneObj.UpdateTransforms();

    const editor::navigation::BakeResult result =
        editor::navigation::BakeNavigationZone(sceneObj, zoneEntity, *assetInstance);
    REQUIRE(result.baked);
    RefPtr<ISerializable> object = assetInstance->ReadObject();
    auto* asset = Cast<pipeline::NavigationZoneAsset>(object.Get());
    REQUIRE(asset != nullptr);
    REQUIRE(pipeline::EnsureNavMeshLoaded(*assetInstance, *asset).IsOk());
    const Span<const byte> blob{reinterpret_cast<const byte*>(asset->navMeshBlob.Data()),
                                asset->navMeshBlob.Size()};
    NavigationTileGridDesc grid;
    REQUIRE(ReadTiledBlobGrid(blob, grid));
    CHECK(grid.countX == 3); // the zone's 40 m, not the plane's 2000 m
    CHECK(grid.countY == 3);
    CHECK(PathAcross(blob, Float3{-15, 0, -15}, Float3{15, 0, 15}));
    RemoveTree(u8"scratch_navbake_plane_db");
}
