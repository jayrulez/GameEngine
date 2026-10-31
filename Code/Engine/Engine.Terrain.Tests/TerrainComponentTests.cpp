// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// engine.terrain: the TerrainComponent's reflected surface and a scene serialize round trip (the
// terrain reference and flags survive save/load through the component manager).
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.scene;
import foundation.scene.resource; // SerializeScene
import foundation.resource;
import foundation.terrain;
import foundation.terrain.resource;
import foundation.heightfield;
import foundation.rhi;
import foundation.rhi.null;
import foundation.render; // GpuRetireQueue (the in-flight-safe rebuild test)
import engine.terrain;

using namespace foundation::core;
using namespace engine::terrain;
namespace scene = foundation::scene;
namespace terrain = foundation::terrain;
namespace tmodel = foundation::terrain;
namespace rhi = foundation::rhi;
namespace hf = foundation::heightfield;

TEST_CASE("engine.terrain: TerrainComponent reflects its authored fields")
{
    RegisterTerrainComponentReflection();
    const TypeInfo& type = TypeOf<TerrainComponent>();
    CHECK(FindProperty(type, "terrain") != nullptr);
    CHECK(FindProperty(type, "castShadows") != nullptr);
    CHECK(FindProperty(type, "visible") != nullptr);
}

TEST_CASE("engine.terrain: a TerrainComponent survives a scene serialize round-trip")
{
    RegisterTerrainComponentReflection();

    scene::Scene a{DefaultAllocator(), u8"terrain-wire"};
    a.AddSystem<TerrainComponentManager>();
    scene::EntityHandle e = a.CreateEntity(u8"terrain");
    {
        TerrainComponent& t = a.GetSystem<TerrainComponentManager>()->Add(e);
        t.terrain.SetId(Guid(42, 7));
        t.castShadows = false;
        t.visible = false;
    }

    MemoryStream stream;
    {
        BinarySerializer writer(stream, SerializeMode::Write);
        scene::SerializeScene(writer, a);
    }
    (void)stream.Seek(0, SeekOrigin::Begin);

    scene::Scene b{DefaultAllocator(), u8"terrain-wire2"};
    b.AddSystem<TerrainComponentManager>();
    {
        BinarySerializer reader(stream, SerializeMode::Read);
        scene::SerializeScene(reader, b);
    }

    scene::EntityHandle loaded = b.FindEntity(a.GetEntityId(e));
    REQUIRE(loaded.IsAssigned());
    TerrainComponent* t = b.GetSystem<TerrainComponentManager>()->Get(loaded);
    REQUIRE(t != nullptr);
    CHECK(t->terrain.id == Guid(42, 7));
    CHECK(t->castShadows == false);
    CHECK(t->visible == false);
}

TEST_CASE("engine.terrain: the GPU height-texture cache keys by heightfield + version")
{
    foundation::rhi::null::NullDevice device{DefaultAllocator()};
    RefPtr<hf::Heightfield> grid =
        MakeRef<hf::Heightfield>(DefaultAllocator(), 65, Float2{64.0f, 64.0f}, 0.0f, 10.0f);
    RefPtr<hf::Heightfield> other =
        MakeRef<hf::Heightfield>(DefaultAllocator(), 65, Float2{64.0f, 64.0f}, 0.0f, 10.0f);

    TerrainHeightTextureCache cache;

    // The first request creates and caches.
    foundation::rhi::TextureView* v1 = cache.GetOrCreate(device, *grid, 1);
    REQUIRE(v1 != nullptr);
    CHECK(cache.Size() == 1u);

    // Same heightfield and version: cache hit, same view, no new entry.
    foundation::rhi::TextureView* v1again = cache.GetOrCreate(device, *grid, 1);
    CHECK(v1again == v1);
    CHECK(cache.Size() == 1u);

    // Same heightfield, bumped version (a sculpt/regen re-upload): rebuilt in place (one entry).
    foundation::rhi::TextureView* v2 = cache.GetOrCreate(device, *grid, 2);
    REQUIRE(v2 != nullptr);
    CHECK(cache.Size() == 1u);

    // A different heightfield: a second cached texture (keyed by the heightfield itself, so two
    // terrains sharing one heightfield share one texture).
    foundation::rhi::TextureView* vb = cache.GetOrCreate(device, *other, 1);
    REQUIRE(vb != nullptr);
    CHECK(cache.Size() == 2u);

    // An empty grid yields no texture.
    RefPtr<hf::Heightfield> empty = MakeRef<hf::Heightfield>(DefaultAllocator());
    CHECK(cache.GetOrCreate(device, *empty, 1) == nullptr);

    cache.Clear(device);
    CHECK(cache.Size() == 0u);
}

TEST_CASE("engine.terrain: the cache keys by UID, never pointer (address-reuse aliasing)")
{
    // The failure mode uid keying prevents: heightfield A dies, a fresh grid B lands on
    // (potentially) the same address at the same version, and the cache must never serve A's
    // texture for B. Pointer keying cannot pass this test reliably; uid keying always does.
    foundation::rhi::null::NullDevice device{DefaultAllocator()};
    TerrainHeightTextureCache cache;

    u64 uidA = 0;
    {
        RefPtr<hf::Heightfield> a =
            MakeRef<hf::Heightfield>(DefaultAllocator(), 65, Float2{64.0f, 64.0f}, 0.0f, 10.0f);
        uidA = a->uid;
        REQUIRE(cache.GetOrCreate(device, *a, a->Version()) != nullptr);
        CHECK(cache.Size() == 1u);
    } // A dies; its cache entry remains keyed by A's uid

    // Fresh grids get fresh uids (the identity that survives address reuse),
    RefPtr<hf::Heightfield> b =
        MakeRef<hf::Heightfield>(DefaultAllocator(), 65, Float2{64.0f, 64.0f}, 0.0f, 10.0f);
    CHECK(b->uid != uidA);
    // so B at the same version gets its own texture entry, never A's stale one.
    foundation::rhi::TextureView* vb = cache.GetOrCreate(device, *b, b->Version());
    REQUIRE(vb != nullptr);
    CHECK(cache.Size() == 2u);

    cache.Clear(device);
}

TEST_CASE("engine.terrain: a version-bump rebuild RETIRES the old texture (in-flight safety)")
{
    // A regenerate or type toggle bumps the version: the old view sits in a submitted frame's
    // descriptor set, so the rebuild routes it through the frame-aged retire queue, never
    // vkDestroyImageView in place (VUID 01026).
    foundation::rhi::null::NullDevice device{DefaultAllocator()};
    foundation::render::GpuRetireQueue retire;
    retire.Initialize(&device, /*framesInFlight*/ 2);

    TerrainHeightTextureCache cache;
    cache.SetRetireQueue(&retire);

    RefPtr<hf::Heightfield> grid =
        MakeRef<hf::Heightfield>(DefaultAllocator(), 65, Float2{64.0f, 64.0f}, 0.0f, 10.0f);
    foundation::rhi::TextureView* v1 = cache.GetOrCreate(device, *grid, grid->Version());
    REQUIRE(v1 != nullptr);
    CHECK(retire.PendingCount() == 0u);

    grid->BumpVersion(); // the regen path
    foundation::rhi::TextureView* v2 = cache.GetOrCreate(device, *grid, grid->Version());
    REQUIRE(v2 != nullptr);
    CHECK(retire.PendingCount() == 2u); // old view and old texture aged, not destroyed in place
    CHECK(cache.Size() == 1u);          // rebuilt in place

    // They free only once every in-flight frame has cycled (framesInFlight + 1 ticks).
    retire.Tick();
    retire.Tick();
    CHECK(retire.PendingCount() == 2u);
    retire.Tick();
    CHECK(retire.PendingCount() == 0u);

    cache.Clear(device); // with a queue wired, Clear retires too (see the next case)
    retire.Flush();
}

TEST_CASE("engine.terrain: Clear RETIRES live entries when a queue is wired (scene destroy mid frame)")
{
    // Stopping play-in-editor destroys the run's scenes from a toolbar click, mid frame loop, while
    // the terrain's height texture is still bound by a submitted frame's descriptor set
    // (VUID-vkDestroyImageView-imageView-01026 if destroyed in place). Clear routes through the
    // frame-aged queue like the version-bump rebuild; without a queue it destroys directly.
    foundation::rhi::null::NullDevice device{DefaultAllocator()};
    foundation::render::GpuRetireQueue retire;
    retire.Initialize(&device, /*framesInFlight*/ 2);
    RefPtr<hf::Heightfield> grid =
        MakeRef<hf::Heightfield>(DefaultAllocator(), 65, Float2{64.0f, 64.0f}, 0.0f, 10.0f);
    {
        TerrainHeightTextureCache cache;
        cache.SetRetireQueue(&retire);
        REQUIRE(cache.GetOrCreate(device, *grid, grid->Version()) != nullptr);
        CHECK(retire.PendingCount() == 0u);
        cache.Clear(device);
        CHECK(cache.Size() == 0u);
        CHECK(retire.PendingCount() == 2u); // the view and the texture aged, not destroyed in place
        retire.Tick();
        retire.Tick();
        CHECK(retire.PendingCount() == 2u);
        retire.Tick();
        CHECK(retire.PendingCount() == 0u); // freed once every in-flight frame cycled
    }
    {
        TerrainHeightTextureCache cache; // no queue: direct destroy (tests, device idled)
        REQUIRE(cache.GetOrCreate(device, *grid, grid->Version()) != nullptr);
        cache.Clear(device);
        CHECK(cache.Size() == 0u);
        CHECK(retire.PendingCount() == 0u);
    }
}

TEST_CASE("engine.terrain: ClearGpu frees the height textures while the device is alive")
{
    // The per-scene manager's cache holds the live height texture, which has to be freed before the
    // device is destroyed. The subsystem calls ClearGpu on scene destroy and at shutdown; this pins
    // the manager-level contract headless.
    foundation::rhi::null::NullDevice device{DefaultAllocator()};

    scene::Scene sceneObj{DefaultAllocator()};
    engine::terrain::AddTerrainSceneManagers(sceneObj);
    auto* mgr = sceneObj.GetSystem<engine::terrain::TerrainComponentManager>();
    REQUIRE(mgr != nullptr);
    mgr->SetRenderContext(&device, 7, nullptr);

    // An in-memory terrain: resource and heightfield assigned directly.
    RefPtr<hf::Heightfield> grid =
        MakeRef<hf::Heightfield>(DefaultAllocator(), 65, Float2{64.0f, 64.0f}, 0.0f, 10.0f);
    auto res = MakeRef<foundation::terrain::TerrainResource>(DefaultAllocator());
    res->heightfield = grid.Get();
    const scene::EntityHandle e = sceneObj.CreateEntity(u8"terrain");
    engine::terrain::TerrainComponent& c = mgr->Add(e);
    c.terrain = res.Get();
    sceneObj.Start();

    foundation::render::ExtractedScene snapshot{DefaultAllocator()};
    mgr->ExtractRenderData(snapshot);
    CHECK(mgr->HeightTextureCount() == 1u); // the extract built and cached the GPU texture

    mgr->ClearGpu();
    CHECK(mgr->HeightTextureCount() == 0u); // freed through the live device

    // Idempotent, and later extracts no-op safely (device nulled).
    mgr->ClearGpu();
    mgr->ExtractRenderData(snapshot);
    CHECK(mgr->HeightTextureCount() == 0u);
}

TEST_CASE("engine.terrain: a cut heightfield extracts its holed chunks' meshes; ClearGpu drops them")
{
    foundation::rhi::null::NullDevice device{DefaultAllocator()};
    scene::Scene sceneObj{DefaultAllocator()};
    engine::terrain::AddTerrainSceneManagers(sceneObj);
    auto* mgr = sceneObj.GetSystem<engine::terrain::TerrainComponentManager>();
    REQUIRE(mgr != nullptr);
    mgr->SetRenderContext(&device, 7, nullptr);
    RefPtr<hf::Heightfield> grid =
        MakeRef<hf::Heightfield>(DefaultAllocator(), 129, Float2{128.0f, 128.0f}, 0.0f, 10.0f);
    auto res = MakeRef<foundation::terrain::TerrainResource>(DefaultAllocator());
    res->heightfield = grid.Get();
    const scene::EntityHandle e = sceneObj.CreateEntity(u8"terrain");
    engine::terrain::TerrainComponent& c = mgr->Add(e);
    c.terrain = res.Get();
    sceneObj.Start();

    foundation::render::ExtractedScene snapshot{DefaultAllocator()};
    mgr->ExtractRenderData(snapshot);
    CHECK(mgr->HoledMeshCount(grid->uid) == 0u); // no holes: no meshes, nothing allocated
    auto* rd = static_cast<const engine::terrain::TerrainRenderData*>(snapshot.Items()[0]);
    CHECK(rd->holedMeshCount == 0u);

    grid->SetHole(100, 100, true); // inside chunk 3 only
    grid->BumpVersion();
    foundation::render::ExtractedScene second{DefaultAllocator()};
    mgr->ExtractRenderData(second);
    CHECK(mgr->HoledMeshCount(grid->uid) == 1u);
    CHECK(mgr->HoleTextureCount() == 1u); // the R8 mask the HOLES shaders sample
    rd = static_cast<const engine::terrain::TerrainRenderData*>(second.Items()[0]);
    CHECK(rd->holeView != nullptr);
    REQUIRE(rd->holedMeshCount == 1u);
    CHECK(rd->holedMeshes[0].chunkIndex == 3u);
    CHECK(rd->holedMeshes[0].indexBuffers[0] != nullptr);
    CHECK(rd->holedMeshes[0].surfaceIndexCounts[0] == 64u * 64u * 6u); // the render rule: one cut sample drops no quad
    CHECK(rd->chunks[3].hasHoles);
    CHECK_FALSE(rd->chunks[0].hasHoles);

    grid->SetHole(100, 100, false); // filled back: the next extract drops the record
    grid->BumpVersion();
    foundation::render::ExtractedScene third{DefaultAllocator()};
    mgr->ExtractRenderData(third);
    CHECK(mgr->HoledMeshCount(grid->uid) == 0u);

    grid->SetHole(5, 5, true);
    grid->BumpVersion();
    foundation::render::ExtractedScene fourth{DefaultAllocator()};
    mgr->ExtractRenderData(fourth);
    CHECK(mgr->HoledMeshCount(grid->uid) == 1u);
    mgr->ClearGpu();
    CHECK(mgr->HoledMeshCount(grid->uid) == 0u);
    CHECK(mgr->HoleTextureCount() == 0u);
}

// The extracted snapshot is self-contained: readable at record time even after the scene, its
// manager and every cache the extract read from are gone (a mid-frame scene mutation can free
// anything borrowed; under ASAN a borrow here is a hard use-after-free).
TEST_CASE("engine.terrain: the extracted snapshot outlives the scene it came from")
{
    foundation::render::ExtractedScene snapshot{DefaultAllocator()};
    {
        rhi::null::NullDevice device{DefaultAllocator()};
        scene::Scene sceneObj{DefaultAllocator()};
        engine::terrain::AddTerrainSceneManagers(sceneObj);
        auto* mgr = sceneObj.GetSystem<engine::terrain::TerrainComponentManager>();
        REQUIRE(mgr != nullptr);
        mgr->SetRenderContext(&device, 7, nullptr);

        RefPtr<foundation::heightfield::Heightfield> grid =
            MakeRef<foundation::heightfield::Heightfield>(DefaultAllocator(), 65,
                                                          Float2{64.0f, 64.0f}, 0.0f, 10.0f);
        auto res = MakeRef<terrain::TerrainResource>(DefaultAllocator());
        res->heightfield = grid.Get();
        const scene::EntityHandle e = sceneObj.CreateEntity(u8"terrain");
        mgr->Add(e).terrain = res.Get();
        sceneObj.Start();

        mgr->ExtractRenderData(snapshot);
        mgr->ClearGpu();
    } // scene, manager, chunk caches and heightfield refs all destroyed here

    REQUIRE(snapshot.Size() == 1u);
    const auto* rd =
        static_cast<const engine::terrain::TerrainRenderData*>(snapshot.Items()[0]);
    REQUIRE(rd->chunks != nullptr);
    REQUIRE(rd->nodes != nullptr);
    REQUIRE(rd->nodeCount > 0u);

    // Cull and LOD straight off the snapshot: the whole grid is visible from above.
    const Float4x4 view =
        Float4x4::LookAtRH(Float3{32.0f, 100.0f, 32.0f}, Float3{32.0f, 0.0f, 32.1f},
                           Float3{0.0f, 0.0f, 1.0f});
    const Float4x4 proj = Float4x4::PerspectiveFovRH(1.2f, 1.0f, 0.1f, 1000.0f);
    const foundation::core::BoundingFrustum frustum(rd->chunkToWorld * (view * proj));
    Array<tmodel::ChunkDraw> draws;
    tmodel::ExtractVisibleChunkDraws(
        Span<const tmodel::TerrainQuadtree::Node>{rd->nodes, rd->nodeCount},
        Span<const tmodel::TerrainChunk>{rd->chunks, rd->chunkCount}, rd->chunkToWorld, view, proj,
        frustum, Span<const f32>{rd->thresholds, rd->thresholdCount}, 0.0f, draws);
    CHECK(!draws.IsEmpty()); // the snapshot culls correctly with no living producer
}

// Navigation bakes from the scene's static geometry sources; a terrain's is its surface: faces
// up, sampled no finer than the detail asked for, holes left open, nothing when the entity is
// inactive or the box misses it.
TEST_CASE("engine.terrain: the surface is static geometry, holes open, faces up")
{
    scene::Scene sceneObj{DefaultAllocator()};
    engine::terrain::AddTerrainSceneManagers(sceneObj);
    auto* mgr = sceneObj.GetSystem<engine::terrain::TerrainComponentManager>();
    REQUIRE(mgr != nullptr);
    RefPtr<hf::Heightfield> grid =
        MakeRef<hf::Heightfield>(DefaultAllocator(), 17, Float2{16.0f, 16.0f}, 0.0f, 10.0f);
    auto res = MakeRef<foundation::terrain::TerrainResource>(DefaultAllocator());
    res->heightfield = grid.Get();
    const scene::EntityHandle e = sceneObj.CreateEntity(u8"terrain");
    mgr->Add(e).terrain = res.Get();
    sceneObj.UpdateTransforms();

    foundation::scene::IStaticGeometrySource* source = mgr->AsStaticGeometrySource();
    REQUIRE(source != nullptr);
    const AABB all{Float3{-20, -20, -20}, Float3{20, 20, 20}};
    Array<Float3> triangles;
    source->CollectStaticGeometry(sceneObj, all, 0.3f, triangles); // finer than the 1 m samples
    CHECK(triangles.Size() == 16u * 16u * 2u * 3u);
    for (usize i = 0; i < triangles.Size(); i += 3)
    {
        const Float3 a = triangles[i + 1] - triangles[i];
        const Float3 b = triangles[i + 2] - triangles[i];
        CHECK(a.z * b.x - a.x * b.z > 0.0f); // the y of cross(a, b): up
    }

    triangles.Clear();
    source->CollectStaticGeometry(sceneObj, all, 2.0f, triangles); // coarser: every 2 samples
    CHECK(triangles.Size() == 8u * 8u * 2u * 3u);

    grid->SetHole(8, 8, true);
    triangles.Clear();
    source->CollectStaticGeometry(sceneObj, all, 0.3f, triangles);
    CHECK(triangles.Size() < 16u * 16u * 2u * 3u);
    CHECK(triangles.Size() >= (16u * 16u - 4u) * 2u * 3u); // only the blocks round the hole

    triangles.Clear();
    source->CollectStaticGeometry(sceneObj, AABB{Float3{30, -5, 30}, Float3{40, 5, 40}}, 0.3f, triangles);
    CHECK(triangles.IsEmpty());
    sceneObj.SetActive(e, false);
    source->CollectStaticGeometry(sceneObj, all, 0.3f, triangles);
    CHECK(triangles.IsEmpty());
}
