// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// engine.vegetation - the component's reflection + wire (an array of layers), and the manager's
// contract: sets built only for chunks in range, one MultiMeshRenderData per (layer, chunk) with
// the fade prefix as its count, region-scoped regrow on a version bump, the build budget, and
// nothing extra without a layer.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

#include <initializer_list>

import foundation.core;
import foundation.scene;
import foundation.geometry;
import foundation.heightfield;
import foundation.terrain;
import foundation.terrain.resource;
import foundation.vegetation.resource;
import foundation.resource;
import foundation.materials;
import foundation.vegetation;
import foundation.render;
import engine.terrain;
import engine.vegetation;

using namespace foundation::core;
namespace scene = foundation::scene;
namespace hf = foundation::heightfield;
namespace tmodel = foundation::terrain;
namespace veg = foundation::vegetation;
namespace render = foundation::render;
namespace geometry = foundation::geometry;
using engine::vegetation::TerrainVegetationComponent;
using engine::vegetation::TerrainVegetationComponentManager;
using engine::vegetation::ProceduralVegetationLayer;
using engine::vegetation::PropVegetationLayer;

namespace
{
    constexpr i32 kGrid = 129;     // 2 x 2 chunks
    constexpr f32 kWorld = 128.0f; // 1 m per quad; chunks are 64 x 64 m, centred on the origin

    RefPtr<hf::Heightfield> MakeFlat(f32 height)
    {
        auto grid = MakeRef<hf::Heightfield>(DefaultAllocator(), kGrid, Float2{kWorld, kWorld},
                                             0.0f, 40.0f);
        const hf::Height sample = grid->WorldYToSample(height);
        for (i32 z = 0; z < kGrid; ++z)
        {
            for (i32 x = 0; x < kGrid; ++x)
            {
                grid->SetSample(x, z, sample);
            }
        }
        return grid;
    }

    // Palette layer 0 one-hot on the left half (x < 0), base on the right.
    RefPtr<tmodel::SplatWeights> MakeHalfSplat()
    {
        constexpr i32 n = 64;
        auto sw = MakeRef<tmodel::SplatWeights>(DefaultAllocator(), n, n);
        Span<u8> idx = sw->Indices();
        Span<u8> wts = sw->Weights();
        for (i32 y = 0; y < n; ++y)
        {
            for (i32 x = 0; x < n / 2; ++x)
            {
                const usize at = sw->TexelOffset(x, y);
                idx[at + 0] = 0;
                wts[at + 0] = 255;
            }
        }
        sw->BumpVersion();
        return sw;
    }

    // A scene with a terrain entity carrying the vegetation component with one grass layer.
    struct Fixture
    {
        scene::Scene scene{DefaultAllocator()};
        RefPtr<hf::Heightfield> grid;
        RefPtr<tmodel::SplatWeights> splat;
        RefPtr<tmodel::TerrainResource> resource;
        RefPtr<geometry::StaticMesh> mesh;
        scene::EntityHandle terrain{};
        TerrainVegetationComponentManager* mgr = nullptr;

        explicit Fixture(bool withSplat = true)
        {
            engine::terrain::AddTerrainSceneManagers(scene);
            engine::vegetation::AddVegetationSceneManagers(scene);
            mgr = scene.GetSystem<TerrainVegetationComponentManager>();
            REQUIRE(mgr != nullptr);

            grid = MakeFlat(2.0f);
            resource = MakeRef<tmodel::TerrainResource>(DefaultAllocator());
            resource->heightfield = grid.Get();
            if (withSplat)
            {
                splat = MakeHalfSplat();
                resource->weights = splat.Get();
            }
            terrain = scene.CreateEntity(u8"terrain");
            scene.GetSystem<engine::terrain::TerrainComponentManager>()->Add(terrain).terrain =
                resource.Get();

            mesh = geometry::Primitives::Cube(DefaultAllocator(), 0.5f);
            TerrainVegetationComponent& c = mgr->Add(terrain);
            ProceduralVegetationLayer grass;
            grass.name = String(u8"Grass");
            grass.mesh = mesh.Get();
            grass.placement = withSplat ? veg::VegetationPlacement::Splat
                                        : veg::VegetationPlacement::Uniform;
            grass.splatLayer = 0;
            grass.density = 0.25f; // 1024 candidates per chunk
            grass.maxSlopeDegrees = 90.0f;
            grass.fadeStart = 40.0f;
            grass.fadeEnd = 80.0f;
            c.proceduralLayers.PushBack(grass);
            scene.Start();
        }

        TerrainVegetationComponent& Component() { return *mgr->Get(terrain); }
        ProceduralVegetationLayer& Layer(usize i = 0) { return Component().proceduralLayers[i]; }
        PropVegetationLayer& Prop(usize i = 0) { return Component().propLayers[i]; }
        // Swap the fixture's grass for one prop layer with the same mesh (the prop tests).
        PropVegetationLayer& MakePropsOnly()
        {
            Component().proceduralLayers.Clear();
            PropVegetationLayer rocks;
            rocks.name = String(u8"Rocks");
            rocks.mesh = mesh.Get();
            rocks.scaleRange = Float2{1.0f, 1.0f};
            rocks.maxSlopeDegrees = 90.0f;
            rocks.fadeStart = 40.0f;
            rocks.fadeEnd = 80.0f;
            Component().propLayers.PushBack(rocks);
            return Prop();
        }

        // Extract with the view at `origin` (or headless), returning the emitted sets.
        Array<const render::MultiMeshRenderData*> Extract(render::ExtractedScene& snapshot,
                                                          const Float3* origin)
        {
            snapshot.Reset();
            if (origin != nullptr)
            {
                snapshot.SetViewOrigin(*origin);
            }
            mgr->ExtractRenderData(snapshot);
            Array<const render::MultiMeshRenderData*> sets;
            for (render::RenderData* item : snapshot.Items())
            {
                REQUIRE(item->kind == render::RenderDataKind::Mesh);
                const auto* md = static_cast<const render::MeshRenderData*>(item);
                REQUIRE(md->multiMesh);
                sets.PushBack(static_cast<const render::MultiMeshRenderData*>(md));
            }
            return sets;
        }
    };

    u32 TotalInstances(const Array<const render::MultiMeshRenderData*>& sets)
    {
        u32 n = 0;
        for (const render::MultiMeshRenderData* s : sets)
        {
            n += s->instanceCount;
        }
        return n;
    }
}

TEST_CASE("engine.vegetation: the component reflects (two lists of reflected layers) and round-trips its wire")
{
    engine::vegetation::RegisterVegetationComponentReflection();
    engine::vegetation::RegisterVegetationComponentReflection(); // idempotent
    const TypeInfo& type = TypeOf<TerrainVegetationComponent>();
    const PropertyInfo* grownList = FindProperty(type, "proceduralLayers");
    REQUIRE(grownList != nullptr);
    REQUIRE(grownList->type != nullptr);
    REQUIRE(grownList->type->container != nullptr); // the list editor's contract
    CHECK(grownList->type->container->elementType == &TypeOf<ProceduralVegetationLayer>());
    const PropertyInfo* propList = FindProperty(type, "propLayers");
    REQUIRE(propList != nullptr);
    REQUIRE(propList->type->container != nullptr);
    CHECK(propList->type->container->elementType == &TypeOf<PropVegetationLayer>());
    CHECK(FindProperty(type, "visible") != nullptr);
    CHECK(FindProperty(type, "mask") != nullptr);
    const Variant* display = FindAttribute(type, "displayName");
    REQUIRE(display != nullptr);
    CHECK(*display->TryGet<String>() == String(u8"Terrain Vegetation"));
    const Variant* category = FindAttribute(type, "category");
    REQUIRE(category != nullptr);
    CHECK(*category->TryGet<String>() == String(u8"Terrain"));

    // A new procedural layer (the inspector's add button default-constructs one) follows the
    // MASK: it grows nothing until the component has a mask with paint on its plane (a Uniform
    // default would grow the moment a mesh is assigned - the 2026-09-22 ruling, kept).
    CHECK(ProceduralVegetationLayer{}.placement == veg::VegetationPlacement::Mask);
    CHECK(ProceduralVegetationLayer{}.ToScatterLayer().placement == veg::VegetationPlacement::Mask);
    CHECK(PropVegetationLayer{}.ToScatterLayer().density == 0.0f); // rules only, no scatter
    const TypeInfo& grownType = TypeOf<ProceduralVegetationLayer>();
    for (const char* name : {"name", "mesh", "materials", "placement", "splatLayer",
                             "splatThreshold", "maskPlane", "density", "scaleRange",
                             "maxSlopeDegrees", "heightRange", "alignToNormal", "fadeStart",
                             "fadeEnd", "castShadows", "maxInstancesPerChunk", "visible",
                             "collisionRadius", "collisionHeight", "collisionGroup"})
    {
        INFO(name);
        CHECK(FindProperty(grownType, name) != nullptr);
    }
    const TypeInfo& propType = TypeOf<PropVegetationLayer>();
    for (const char* name : {"name", "mesh", "materials", "scaleRange", "maxSlopeDegrees",
                             "heightRange", "alignToNormal", "fadeStart", "fadeEnd",
                             "castShadows", "maxInstancesPerChunk", "visible", "collisionRadius",
                             "collisionHeight", "collisionGroup"})
    {
        INFO(name);
        CHECK(FindProperty(propType, name) != nullptr);
    }
    CHECK(FindProperty(propType, "placement") == nullptr); // a prop layer has no source
    CHECK(FindProperty(propType, "density") == nullptr);
    CHECK(FindProperty(propType, "instances") == nullptr); // the brush is its editor
    CHECK(FindProperty(type, "proceduralLayers") != nullptr);
    CHECK(FindProperty(type, "propLayers") != nullptr);
    CHECK(FindProperty(type, "layers") == nullptr); // the one list is gone (data version 2)
    CHECK(type.dataVersion == 4u); // 3: a material per slot; 4: a layer's collision
    CHECK(type.minReadDataVersion == 1u); // the legacy reader for the one-list layout

    TerrainVegetationComponent authored;
    authored.visible = false;
    authored.mask.SetId(Guid{0x77u, 0x88u});
    ProceduralVegetationLayer grass;
    grass.name = String(u8"Grass");
    grass.placement = veg::VegetationPlacement::Uniform;
    grass.splatLayer = 3;
    grass.splatThreshold = 0.5f;
    grass.density = 7.5f;
    grass.scaleRange = Float2{0.5f, 2.5f};
    grass.maxSlopeDegrees = 12.0f;
    grass.heightRange = Float2{-3.0f, 30.0f};
    grass.alignToNormal = true;
    grass.fadeStart = 10.0f;
    grass.fadeEnd = 20.0f;
    grass.castShadows = true;
    grass.maxInstancesPerChunk = 512;
    grass.visible = false;
    authored.proceduralLayers.PushBack(grass);
    PropVegetationLayer rocks;
    rocks.name = String(u8"Rocks");
    rocks.castShadows = true;
    rocks.instances.PushBack(Float4x4::Translation(Float3{1.0f, 2.0f, 3.0f}));
    rocks.instances.PushBack(Float4x4::Translation(Float3{7.0f, 2.0f, 3.0f}));
    authored.propLayers.PushBack(rocks);

    MemoryStream buffer;
    {
        BinarySerializer writer(buffer, SerializeMode::Write);
        Serialize(writer, authored);
    }
    (void)buffer.Seek(0, SeekOrigin::Begin);
    TerrainVegetationComponent loaded;
    {
        BinarySerializer reader(buffer, SerializeMode::Read);
        Serialize(reader, loaded);
    }
    CHECK(!loaded.visible);
    CHECK(loaded.mask.id == Guid{0x77u, 0x88u});
    REQUIRE(loaded.proceduralLayers.Size() == 1u);
    const ProceduralVegetationLayer& g = loaded.proceduralLayers[0];
    CHECK(g.name == String(u8"Grass"));
    CHECK(g.placement == veg::VegetationPlacement::Uniform);
    CHECK(g.splatLayer == 3u);
    CHECK(g.splatThreshold == 0.5f);
    CHECK(g.density == 7.5f);
    CHECK(g.scaleRange.y == 2.5f);
    CHECK(g.maxSlopeDegrees == 12.0f);
    CHECK(g.heightRange.x == -3.0f);
    CHECK(g.alignToNormal);
    CHECK(g.fadeStart == 10.0f);
    CHECK(g.fadeEnd == 20.0f);
    CHECK(g.castShadows);
    CHECK(g.maxInstancesPerChunk == 512u);
    CHECK(!g.visible);
    REQUIRE(loaded.propLayers.Size() == 1u);
    CHECK(loaded.propLayers[0].name == String(u8"Rocks"));
    CHECK(loaded.propLayers[0].castShadows);
    REQUIRE(loaded.propLayers[0].instances.Size() == 2u); // the authored props ride the wire
    CHECK(loaded.propLayers[0].instances[1].m[3][0] == 7.0f);
}

namespace
{
    // The data-version-1 layout of a layer (one list, every field, `placement` 3 = Scattered),
    // written the way a 2026-09-22 build wrote it: the legacy reader's fixture.
    struct LayerV1Fixture
    {
        String name;
        Guid meshId;
        u8 placement = 3;
        f32 density = 2.0f;
        u32 maskPlane = 0;
        Array<Float4x4> instances;
    };
    void Serialize(ISerializer& ar, LayerV1Fixture& l)
    {
        foundation::resource::Ref<geometry::StaticMesh> mesh;
        mesh.SetId(l.meshId);
        foundation::resource::Ref<foundation::materials::Material> material;
        u32 splatLayer = 0;
        f32 splatThreshold = 0.25f;
        Float2 scaleRange{0.8f, 1.2f};
        f32 maxSlopeDegrees = 35.0f;
        Float2 heightRange{-1.0e6f, 1.0e6f};
        bool alignToNormal = false;
        f32 fadeStart = 40.0f;
        f32 fadeEnd = 80.0f;
        bool castShadows = false;
        u32 maxInstancesPerChunk = 4096;
        bool visible = true;
        foundation::core::Serialize(ar, "name", l.name);
        foundation::core::Serialize(ar, "mesh", mesh);
        foundation::core::Serialize(ar, "material", material);
        foundation::core::Serialize(ar, "placement", l.placement);
        foundation::core::Serialize(ar, "splatLayer", splatLayer);
        foundation::core::Serialize(ar, "splatThreshold", splatThreshold);
        foundation::core::Serialize(ar, "maskPlane", l.maskPlane);
        foundation::core::Serialize(ar, "density", l.density);
        foundation::core::Serialize(ar, "scaleRange", scaleRange);
        foundation::core::Serialize(ar, "maxSlopeDegrees", maxSlopeDegrees);
        foundation::core::Serialize(ar, "heightRange", heightRange);
        foundation::core::Serialize(ar, "alignToNormal", alignToNormal);
        foundation::core::Serialize(ar, "fadeStart", fadeStart);
        foundation::core::Serialize(ar, "fadeEnd", fadeEnd);
        foundation::core::Serialize(ar, "castShadows", castShadows);
        foundation::core::Serialize(ar, "maxInstancesPerChunk", maxInstancesPerChunk);
        foundation::core::Serialize(ar, "visible", visible);
        foundation::core::Serialize(ar, "instances", l.instances);
    }
}

TEST_CASE("engine.vegetation: the legacy reader splits a data-version-1 one-list payload into the two lists")
{
    // A component record as a 2026-09-22 scene stored it: the chain stamped version 1, then
    // `layers` (Cube: Scattered with two instances; Grass: Mask on plane 0), `mask`, `visible`.
    // The reader (ReadsDataVersionsFrom(1)) accepts it, the body branches on ar.Version() == 1
    // and splits by placement; a re-save writes version 2 (the RTHomes1 scene's path).
    engine::vegetation::RegisterVegetationComponentReflection();
    const TypeInfo& type = TypeOf<TerrainVegetationComponent>();
    MemoryStream stream;
    {
        BinarySerializer ar(stream, SerializeMode::Write);
        const SerializedDataVersion chain[] = {{type.id, 1u}};
        u32 n = 1;
        ar.Key("dataVersions");
        ar.BeginArray(n);
        SerializedDataVersion entry = chain[0];
        ar.Key("type");
        ar.Scalar(&entry.typeId, ScalarKind::UInt64);
        ar.Key("version");
        ar.Scalar(&entry.version, ScalarKind::UInt32);
        ar.EndArray();
        ar.PushVersionScope(chain, 1);
        Array<LayerV1Fixture> layers;
        LayerV1Fixture cube;
        cube.name = String(u8"Cube");
        cube.meshId = Guid{0x11u, 0x22u};
        cube.placement = 3; // Scattered
        cube.instances.PushBack(Float4x4::Translation(Float3{5.0f, 1.0f, 5.0f}));
        cube.instances.PushBack(Float4x4::Translation(Float3{-5.0f, 1.0f, 5.0f}));
        layers.PushBack(cube);
        LayerV1Fixture grass;
        grass.name = String(u8"Grass");
        grass.meshId = Guid{0x33u, 0x44u};
        grass.placement = 2; // Mask
        grass.density = 2.0f;
        grass.maskPlane = 0;
        layers.PushBack(grass);
        foundation::core::Serialize(ar, "layers", layers);
        foundation::resource::Ref<veg::VegetationMask> mask;
        mask.SetId(Guid{0x55u, 0x66u});
        foundation::core::Serialize(ar, "mask", mask);
        bool visible = true;
        foundation::core::Serialize(ar, "visible", visible);
        ar.PopVersionScope();
        REQUIRE(ar.IsOk());
    }
    REQUIRE(stream.Seek(0, SeekOrigin::Begin) == 0);
    TerrainVegetationComponent loaded;
    {
        BinarySerializer ar(stream, SerializeMode::Read);
        BeginVersionedPayload(ar, type);
        CHECK(ar.Version() == 1u);
        Serialize(ar, loaded);
        EndVersionedPayload(ar);
        REQUIRE(ar.IsOk());
    }
    REQUIRE(loaded.proceduralLayers.Size() == 1u);
    CHECK(loaded.proceduralLayers[0].name == String(u8"Grass"));
    CHECK(loaded.proceduralLayers[0].placement == veg::VegetationPlacement::Mask);
    CHECK(loaded.proceduralLayers[0].density == 2.0f);
    CHECK(loaded.proceduralLayers[0].mesh.id == Guid{0x33u, 0x44u});
    REQUIRE(loaded.propLayers.Size() == 1u);
    CHECK(loaded.propLayers[0].name == String(u8"Cube"));
    CHECK(loaded.propLayers[0].mesh.id == Guid{0x11u, 0x22u});
    REQUIRE(loaded.propLayers[0].instances.Size() == 2u);
    CHECK(loaded.propLayers[0].instances[1].m[3][0] == -5.0f);
    CHECK(loaded.mask.id == Guid{0x55u, 0x66u});
    CHECK(loaded.visible);
    // The same body under the CURRENT version writes and reads the two lists (version 4).
    MemoryStream again;
    {
        BinarySerializer ar(again, SerializeMode::Write);
        BeginVersionedPayload(ar, type);
        Serialize(ar, loaded);
        EndVersionedPayload(ar);
        REQUIRE(ar.IsOk());
    }
    REQUIRE(again.Seek(0, SeekOrigin::Begin) == 0);
    TerrainVegetationComponent resaved;
    {
        BinarySerializer ar(again, SerializeMode::Read);
        BeginVersionedPayload(ar, type);
        CHECK(ar.Version() == 4u);
        Serialize(ar, resaved);
        EndVersionedPayload(ar);
        REQUIRE(ar.IsOk());
    }
    CHECK(resaved.proceduralLayers.Size() == 1u);
    CHECK(resaved.propLayers.Size() == 1u);
    CHECK(resaved.propLayers[0].instances.Size() == 2u);
}

namespace
{
    // A procedural layer in the data-version-2 layout (one optional `material`), as a scene saved
    // before 2026-10-05 stored it.
    void WriteLayerV2(ISerializer& ar, const Guid& meshId, const Guid& materialId)
    {
        String name(u8"Pines");
        foundation::resource::Ref<geometry::StaticMesh> mesh;
        mesh.SetId(meshId);
        foundation::resource::Ref<foundation::materials::Material> material;
        material.SetId(materialId);
        Float2 scaleRange{0.8f, 1.2f};
        f32 maxSlopeDegrees = 35.0f;
        Float2 heightRange{-1.0e6f, 1.0e6f};
        bool alignToNormal = false;
        f32 fadeStart = 40.0f;
        f32 fadeEnd = 80.0f;
        bool castShadows = true;
        u32 maxInstancesPerChunk = 4096;
        bool visible = true;
        u8 placement = 1; // Splat
        u32 splatLayer = 2;
        f32 splatThreshold = 0.5f;
        u32 maskPlane = 0;
        f32 density = 0.025f;
        foundation::core::Serialize(ar, "name", name);
        foundation::core::Serialize(ar, "mesh", mesh);
        foundation::core::Serialize(ar, "material", material);
        foundation::core::Serialize(ar, "scaleRange", scaleRange);
        foundation::core::Serialize(ar, "maxSlopeDegrees", maxSlopeDegrees);
        foundation::core::Serialize(ar, "heightRange", heightRange);
        foundation::core::Serialize(ar, "alignToNormal", alignToNormal);
        foundation::core::Serialize(ar, "fadeStart", fadeStart);
        foundation::core::Serialize(ar, "fadeEnd", fadeEnd);
        foundation::core::Serialize(ar, "castShadows", castShadows);
        foundation::core::Serialize(ar, "maxInstancesPerChunk", maxInstancesPerChunk);
        foundation::core::Serialize(ar, "visible", visible);
        foundation::core::Serialize(ar, "placement", placement);
        foundation::core::Serialize(ar, "splatLayer", splatLayer);
        foundation::core::Serialize(ar, "splatThreshold", splatThreshold);
        foundation::core::Serialize(ar, "maskPlane", maskPlane);
        foundation::core::Serialize(ar, "density", density);
    }
}

TEST_CASE("engine.vegetation: a data-version-2 layer's one material reads as its list's one entry")
{
    engine::vegetation::RegisterVegetationComponentReflection();
    const TypeInfo& type = TypeOf<TerrainVegetationComponent>();
    MemoryStream stream;
    {
        BinarySerializer ar(stream, SerializeMode::Write);
        const SerializedDataVersion chain[] = {{type.id, 2u}};
        u32 n = 1;
        ar.Key("dataVersions");
        ar.BeginArray(n);
        SerializedDataVersion entry = chain[0];
        ar.Key("type");
        ar.Scalar(&entry.typeId, ScalarKind::UInt64);
        ar.Key("version");
        ar.Scalar(&entry.version, ScalarKind::UInt32);
        ar.EndArray();
        ar.PushVersionScope(chain, 1);
        u32 count = 2;
        ar.Key("proceduralLayers");
        ar.BeginArray(count);
        WriteLayerV2(ar, Guid{0x11u, 0x22u}, Guid{0x33u, 0x44u});
        WriteLayerV2(ar, Guid{0x11u, 0x22u}, Guid{}); // no material: an empty list
        ar.EndArray();
        Array<PropVegetationLayer> none;
        foundation::core::Serialize(ar, "propLayers", none);
        foundation::resource::Ref<veg::VegetationMask> mask;
        foundation::core::Serialize(ar, "mask", mask);
        bool visible = true;
        foundation::core::Serialize(ar, "visible", visible);
        ar.PopVersionScope();
        REQUIRE(ar.IsOk());
    }
    REQUIRE(stream.Seek(0, SeekOrigin::Begin) == 0);
    TerrainVegetationComponent loaded;
    {
        BinarySerializer ar(stream, SerializeMode::Read);
        BeginVersionedPayload(ar, type);
        CHECK(ar.Version() == 2u);
        Serialize(ar, loaded);
        EndVersionedPayload(ar);
        REQUIRE(ar.IsOk());
    }
    REQUIRE(loaded.proceduralLayers.Size() == 2u);
    REQUIRE(loaded.proceduralLayers[0].materials.Size() == 1u);
    CHECK(loaded.proceduralLayers[0].materials[0].id == Guid{0x33u, 0x44u});
    CHECK(loaded.proceduralLayers[0].density == 0.025f); // the fields after it still line up
    CHECK(loaded.proceduralLayers[0].splatLayer == 2u);
    CHECK(loaded.proceduralLayers[1].materials.IsEmpty());
}

// Snowline's pines (bark, two needle greens, snow) drew white: a layer had one material and the
// renderer was never told the mesh's slots. One entry covers the mesh; more route the submeshes.
TEST_CASE("engine.vegetation: a layer's materials reach the renderer per slot, one covering the mesh")
{
    Fixture f;
    f.mgr->SetBuildBudget(100);
    render::ExtractedScene snapshot{DefaultAllocator()};
    RefPtr<foundation::materials::Material> bark =
        foundation::materials::CreatePBR(u8"bark", Float4{0.2f, 0.1f, 0.05f, 1.0f}, 0.0f, 0.9f);
    RefPtr<foundation::materials::Material> needles =
        foundation::materials::CreatePBR(u8"needles", Float4{0.05f, 0.2f, 0.1f, 1.0f}, 0.0f, 0.9f);

    // One material: the whole-mesh path, no submesh routing (batching kept).
    f.Layer().materials.PushBack(foundation::resource::Ref<foundation::materials::Material>(bark));
    Array<const render::MultiMeshRenderData*> sets = f.Extract(snapshot, nullptr);
    REQUIRE(!sets.IsEmpty());
    for (const render::MultiMeshRenderData* s : sets)
    {
        CHECK(s->material == bark.Get());
        CHECK(s->submeshMaterials == nullptr);
        CHECK(s->submeshMaterialCount == 0u);
    }

    // Two: slot 0 is the primary, and the renderer gets the list per slot.
    f.Layer().materials.PushBack(foundation::resource::Ref<foundation::materials::Material>(needles));
    sets = f.Extract(snapshot, nullptr);
    REQUIRE(!sets.IsEmpty());
    for (const render::MultiMeshRenderData* s : sets)
    {
        CHECK(s->material == bark.Get());
        REQUIRE(s->submeshMaterialCount == 2u);
        CHECK(s->submeshMaterials[0].Get() == bark.Get());
        CHECK(s->submeshMaterials[1].Get() == needles.Get());
    }

    // None: the default material (null), as before.
    f.Layer().materials.Clear();
    sets = f.Extract(snapshot, nullptr);
    for (const render::MultiMeshRenderData* s : sets)
    {
        CHECK(s->material == nullptr);
        CHECK(s->submeshMaterials == nullptr);
    }
}

TEST_CASE("engine.vegetation: one set per (layer, chunk) in range; the splat picks the chunks; shadows follow the layer")
{
    Fixture f;
    f.mgr->SetBuildBudget(100);
    render::ExtractedScene snapshot{DefaultAllocator()};

    // Headless (no view origin): every chunk is in range; only the painted half grows, so the
    // two x < 0 chunks emit and the two x > 0 chunks scatter to nothing.
    Array<const render::MultiMeshRenderData*> sets = f.Extract(snapshot, nullptr);
    CHECK(f.mgr->BuildCount() == 4u); // all four chunks scattered
    REQUIRE(sets.Size() == 2u);
    CHECK(f.mgr->BuiltSetCount() == 2u);
    CHECK(f.mgr->InstanceCount() == TotalInstances(sets));
    for (const render::MultiMeshRenderData* s : sets)
    {
        CHECK(s->key != 0u);
        CHECK(s->instanceCount > 0u);
        CHECK(s->instanceCount == 1024u); // Splat share 1 on the painted half keeps every candidate
        CHECK(s->transforms != nullptr);
        CHECK(s->mesh == f.mesh.Get());
        CHECK(s->version > 0u);
        CHECK(!s->castShadows); // grass default
        CHECK(s->category == render::RenderCategories::Opaque);
        CHECK(s->worldCenter.x < 0.0f); // the painted (x < 0) chunks
        CHECK(s->worldRadius > 32.0f);
        CHECK(render::EntityTag::Index(s->entityId) == f.terrain.index);
        for (u32 i = 0; i < s->instanceCount; ++i)
        {
            CHECK(s->transforms[i].m[3][0] < 0.0f);
            CHECK(s->transforms[i].m[3][1] == doctest::Approx(2.0f).epsilon(0.01));
        }
    }
    CHECK(sets[0]->key != sets[1]->key);

    // A rock layer casts.
    f.Layer().castShadows = true;
    sets = f.Extract(snapshot, nullptr);
    REQUIRE(sets.Size() == 2u);
    CHECK(sets[0]->castShadows);
    CHECK(f.mgr->BuildCount() == 4u); // fade/shadow changes never rescatter

    // A second extraction re-emits the same sets: same keys, same version (no re-upload).
    const u64 key0 = sets[0]->key;
    const u32 version0 = sets[0]->version;
    sets = f.Extract(snapshot, nullptr);
    REQUIRE(sets.Size() == 2u);
    CHECK(sets[0]->key == key0);
    CHECK(sets[0]->version == version0);
}

TEST_CASE("engine.vegetation: the fade prefix thins by distance and out-of-range chunks are absent")
{
    Fixture f(/*withSplat*/ false); // Uniform: all four chunks grow
    f.mgr->SetBuildBudget(100);
    render::ExtractedScene snapshot{DefaultAllocator()};

    // The view over the (-32, -32) chunk's centre, 10 m up: that chunk is at distance 0, the
    // diagonal one (32, 32) is ~ 90 - 45 = 45 m from its bounds (inside the fade), so it
    // draws a partial prefix; the near chunks are full.
    const Float3 near{-32.0f, 12.0f, -32.0f};
    Array<const render::MultiMeshRenderData*> sets = f.Extract(snapshot, &near);
    REQUIRE(sets.Size() == 4u);
    u32 full = 0;
    u32 partial = 0;
    for (const render::MultiMeshRenderData* s : sets)
    {
        CHECK(s->instanceCount > 0u);
        CHECK(s->instanceCount <= 1024u);
        CHECK(s->fadeStart == doctest::Approx(40.0f)); // the layer's window, for the per-instance dissolve
        CHECK(s->fadeEnd == doctest::Approx(80.0f));
        if (s->instanceCount == 1024u)
        {
            ++full;
        }
        else
        {
            ++partial;
        }
    }
    CHECK(full >= 1u);
    CHECK(partial >= 1u);

    // Far away: nothing is in range and nothing new is built (a hidden viewport never grows).
    const u64 builds = f.mgr->BuildCount();
    const Float3 far{2000.0f, 12.0f, 0.0f};
    sets = f.Extract(snapshot, &far);
    CHECK(sets.IsEmpty());
    CHECK(f.mgr->BuildCount() == builds);
    CHECK(f.mgr->BuiltSetCount() == 4u); // the sets stay cached for the return

    // Back in range: the cached sets return without a rebuild.
    sets = f.Extract(snapshot, &near);
    CHECK(sets.Size() == 4u);
    CHECK(f.mgr->BuildCount() == builds);
}

TEST_CASE("engine.vegetation: a version bump regrows only the touched chunks when a region says which")
{
    Fixture f(/*withSplat*/ false);
    f.mgr->SetBuildBudget(100);
    render::ExtractedScene snapshot{DefaultAllocator()};
    Array<const render::MultiMeshRenderData*> sets = f.Extract(snapshot, nullptr);
    REQUIRE(sets.Size() == 4u);
    CHECK(f.mgr->BuildCount() == 4u);
    Array<u32> versions;
    Array<u64> keys;
    for (const render::MultiMeshRenderData* s : sets)
    {
        versions.PushBack(s->version);
        keys.PushBack(s->key);
    }

    // A sculpt inside chunk (0, 0) only: bump + region -> ONE rebuild, its version bumps, the
    // other three keep theirs.
    f.grid->BumpVersion();
    hf::HeightfieldRegion region;
    region.minX = 10;
    region.maxX = 20;
    region.minZ = 10;
    region.maxZ = 20;
    f.mgr->InvalidateRegion(region);
    sets = f.Extract(snapshot, nullptr);
    REQUIRE(sets.Size() == 4u);
    CHECK(f.mgr->BuildCount() == 5u);
    u32 bumped = 0;
    for (const render::MultiMeshRenderData* s : sets)
    {
        for (usize i = 0; i < keys.Size(); ++i)
        {
            if (keys[i] == s->key)
            {
                bumped += (s->version != versions[i]) ? 1 : 0;
            }
        }
    }
    CHECK(bumped == 1u);

    // A bump with no region notice regrows everything (the conservative fallback).
    f.grid->BumpVersion();
    sets = f.Extract(snapshot, nullptr);
    CHECK(f.mgr->BuildCount() == 9u);

    // A splat bump behaves the same; a scatter-parameter change resets the whole layer.
    auto splat = MakeHalfSplat();
    f.resource->weights = splat.Get();
    sets = f.Extract(snapshot, nullptr); // a new splat identity: every chunk regrows
    CHECK(f.mgr->BuildCount() == 13u);
    f.Layer().density = 0.5f;
    sets = f.Extract(snapshot, nullptr);
    CHECK(f.mgr->BuildCount() == 17u);
    for (const render::MultiMeshRenderData* s : sets)
    {
        CHECK(s->instanceCount == 2048u);
    }

    // Moving the terrain entity recomposes (versions bump) without a rescatter.
    f.scene.SetLocalPosition(f.terrain, Float3{100.0f, 0.0f, 0.0f});
    f.scene.UpdateTransforms();
    sets = f.Extract(snapshot, nullptr);
    CHECK(f.mgr->BuildCount() == 17u);
    REQUIRE(sets.Size() == 4u);
    for (const render::MultiMeshRenderData* s : sets)
    {
        CHECK(s->worldCenter.x > 30.0f); // shifted by +100
        CHECK(s->transforms[0].m[3][0] > 30.0f);
    }
}

TEST_CASE("engine.vegetation: the build budget spreads a cold start over extractions")
{
    Fixture f(/*withSplat*/ false);
    f.mgr->SetBuildBudget(1);
    render::ExtractedScene snapshot{DefaultAllocator()};
    Array<const render::MultiMeshRenderData*> sets = f.Extract(snapshot, nullptr);
    CHECK(sets.Size() == 1u);
    CHECK(f.mgr->BuildCount() == 1u);
    sets = f.Extract(snapshot, nullptr);
    CHECK(sets.Size() == 2u);
    sets = f.Extract(snapshot, nullptr);
    sets = f.Extract(snapshot, nullptr);
    CHECK(sets.Size() == 4u);
    CHECK(f.mgr->BuildCount() == 4u);
    sets = f.Extract(snapshot, nullptr);
    CHECK(f.mgr->BuildCount() == 4u); // warm: no more builds
}

TEST_CASE("engine.vegetation: two layers are two families of sets; a removed slot drops its sets")
{
    Fixture f(/*withSplat*/ false);
    f.mgr->SetBuildBudget(100);
    RefPtr<geometry::StaticMesh> rockMesh = geometry::Primitives::Cube(DefaultAllocator(), 1.5f);
    ProceduralVegetationLayer rocks;
    rocks.name = String(u8"Rocks");
    rocks.mesh = rockMesh.Get();
    rocks.placement = veg::VegetationPlacement::Uniform;
    rocks.density = 0.01f; // ~41 per chunk
    rocks.maxSlopeDegrees = 90.0f;
    rocks.castShadows = true;
    f.Component().proceduralLayers.PushBack(rocks);

    render::ExtractedScene snapshot{DefaultAllocator()};
    Array<const render::MultiMeshRenderData*> sets = f.Extract(snapshot, nullptr);
    REQUIRE(sets.Size() == 8u); // 4 chunks x 2 layers
    CHECK(f.mgr->BuildCount() == 8u);
    u32 grassSets = 0;
    u32 rockSets = 0;
    for (const render::MultiMeshRenderData* s : sets)
    {
        if (s->mesh == f.mesh.Get())
        {
            ++grassSets;
            CHECK(!s->castShadows);
            CHECK(s->instanceCount == 1024u);
        }
        else
        {
            REQUIRE(s->mesh == rockMesh.Get());
            ++rockSets;
            CHECK(s->castShadows);
            CHECK(s->instanceCount == 41u);
        }
        // Every set's key is unique across layers and chunks.
        for (const render::MultiMeshRenderData* other : sets)
        {
            CHECK((other == s || other->key != s->key));
        }
    }
    CHECK(grassSets == 4u);
    CHECK(rockSets == 4u);

    // A hidden layer draws nothing but keeps its sets; unhiding costs no rebuild.
    f.Layer(1).visible = false;
    sets = f.Extract(snapshot, nullptr);
    CHECK(sets.Size() == 4u);
    CHECK(f.mgr->BuiltSetCount() == 8u);
    f.Layer(1).visible = true;
    sets = f.Extract(snapshot, nullptr);
    CHECK(sets.Size() == 8u);
    CHECK(f.mgr->BuildCount() == 8u);

    // Removing the rock slot drops its sets on the next extraction.
    f.Component().proceduralLayers.RemoveAt(1);
    sets = f.Extract(snapshot, nullptr);
    CHECK(sets.Size() == 4u);
    CHECK(f.mgr->BuiltSetCount() == 4u);
}

TEST_CASE("engine.vegetation: no component, a hidden component, one off any terrain, or a layer without a mesh extracts nothing")
{
    scene::Scene bare{DefaultAllocator()};
    engine::terrain::AddTerrainSceneManagers(bare);
    engine::vegetation::AddVegetationSceneManagers(bare);
    auto* mgr = bare.GetSystem<TerrainVegetationComponentManager>();
    REQUIRE(mgr != nullptr);
    bare.Start();
    render::ExtractedScene snapshot{DefaultAllocator()};
    mgr->ExtractRenderData(snapshot);
    CHECK(snapshot.IsEmpty());

    Fixture f(/*withSplat*/ false);
    f.mgr->SetBuildBudget(100);
    f.Component().visible = false;
    Array<const render::MultiMeshRenderData*> sets = f.Extract(snapshot, nullptr);
    CHECK(sets.IsEmpty());
    f.Component().visible = true;
    sets = f.Extract(snapshot, nullptr);
    CHECK(sets.Size() == 4u);

    // An inactive entity is absent.
    f.scene.SetActive(f.terrain, false);
    sets = f.Extract(snapshot, nullptr);
    CHECK(sets.IsEmpty());
    f.scene.SetActive(f.terrain, true);
    sets = f.Extract(snapshot, nullptr);
    CHECK(sets.Size() == 4u);

    // The component on a CHILD of the terrain entity grows on the parent's terrain; on an
    // entity with no terrain above it, nothing.
    const scene::EntityHandle child = f.scene.CreateEntity(u8"dressing");
    f.scene.SetParent(child, f.terrain);
    TerrainVegetationComponent moved = f.Component();
    f.mgr->Remove(f.terrain);
    f.mgr->Add(child) = moved;
    sets = f.Extract(snapshot, nullptr);
    CHECK(sets.Size() == 4u);
    f.scene.SetParent(child, scene::EntityHandle{});
    sets = f.Extract(snapshot, nullptr);
    CHECK(sets.IsEmpty());
    f.scene.SetParent(child, f.terrain);
    sets = f.Extract(snapshot, nullptr);
    CHECK(sets.Size() == 4u);

    // No mesh: nothing to instance.
    f.mgr->Get(child)->proceduralLayers[0].mesh = nullptr;
    sets = f.Extract(snapshot, nullptr);
    CHECK(sets.IsEmpty());

    // A removed component drops its caches.
    f.mgr->Get(child)->proceduralLayers[0].mesh = f.mesh.Get();
    sets = f.Extract(snapshot, nullptr);
    CHECK(f.mgr->BuiltSetCount() == 4u);
    f.mgr->Remove(child);
    sets = f.Extract(snapshot, nullptr);
    CHECK(sets.IsEmpty());
    CHECK(f.mgr->BuiltSetCount() == 0u);
}

TEST_CASE("engine.vegetation: a Mask layer follows the component's painted plane; a mask edit regrows the touched chunks")
{
    Fixture f(/*withSplat*/ false);
    f.mgr->SetBuildBudget(100);
    // Plane 0 painted on the z < 0 half of the footprint.
    auto mask = MakeRef<veg::VegetationMask>(DefaultAllocator(), 64, 64, 1);
    for (i32 y = 0; y < 32; ++y)
    {
        for (i32 x = 0; x < 64; ++x)
        {
            mask->SetDensity(0, x, y, 255);
        }
    }
    f.Component().mask = mask.Get();
    f.Layer().placement = veg::VegetationPlacement::Mask;
    f.Layer().maskPlane = 0;

    render::ExtractedScene snapshot{DefaultAllocator()};
    Array<const render::MultiMeshRenderData*> sets = f.Extract(snapshot, nullptr);
    REQUIRE(sets.Size() == 2u); // the two z < 0 chunks
    for (const render::MultiMeshRenderData* s : sets)
    {
        CHECK(s->worldCenter.z < 0.0f);
        CHECK(s->instanceCount == 1024u);
    }
    CHECK(f.mgr->BuildCount() == 4u);

    // A brush stroke over one chunk's footprint quarter, with the footprint notice: only that
    // chunk regrows (its instance count follows the new paint).
    for (i32 y = 32; y < 64; ++y)
    {
        for (i32 x = 32; x < 64; ++x)
        {
            mask->SetDensity(0, x, y, 255); // paint the (+x, +z) quadrant
        }
    }
    mask->BumpVersion();
    // The notice stays inside the chunk: a rect touching the shared boundary sample (u = 0.5)
    // would rightly regrow both neighbours.
    f.mgr->InvalidateFootprint(0.6f, 0.6f, 0.9f, 0.9f, kGrid);
    sets = f.Extract(snapshot, nullptr);
    CHECK(f.mgr->BuildCount() == 5u);
    REQUIRE(sets.Size() == 3u);

    // A bump with no notice regrows everything; dropping the mask empties the Mask layer.
    mask->BumpVersion();
    sets = f.Extract(snapshot, nullptr);
    CHECK(f.mgr->BuildCount() == 9u);
    CHECK(sets.Size() == 3u);
    f.Component().mask = nullptr;
    sets = f.Extract(snapshot, nullptr);
    CHECK(sets.IsEmpty());
}

TEST_CASE("engine.vegetation: a prop layer draws its authored instances bucketed per chunk; a change re-buckets")
{
    Fixture f(/*withSplat*/ false);
    f.mgr->SetBuildBudget(100);
    PropVegetationLayer& rocks = f.MakePropsOnly();
    // Three props in the (-x, -z) chunk, one in the (+x, +z) chunk, none elsewhere.
    rocks.instances.PushBack(Float4x4::Translation(Float3{-40.0f, 2.0f, -40.0f}));
    rocks.instances.PushBack(Float4x4::Translation(Float3{-10.0f, 2.0f, -50.0f}));
    rocks.instances.PushBack(Float4x4::Translation(Float3{-30.0f, 2.0f, -1.0f}));
    rocks.instances.PushBack(Float4x4::Translation(Float3{20.0f, 2.0f, 20.0f}));

    render::ExtractedScene snapshot{DefaultAllocator()};
    Array<const render::MultiMeshRenderData*> sets = f.Extract(snapshot, nullptr);
    REQUIRE(sets.Size() == 2u);
    u32 three = 0, one = 0;
    for (const render::MultiMeshRenderData* s : sets)
    {
        if (s->instanceCount == 3u)
        {
            ++three;
            CHECK(s->worldCenter.x < 0.0f);
            CHECK(s->worldCenter.z < 0.0f);
            for (u32 i = 0; i < 3; ++i)
            {
                CHECK(s->transforms[i].m[3][0] < 0.0f);
            }
        }
        else if (s->instanceCount == 1u)
        {
            ++one;
            CHECK(s->transforms[0].m[3][0] == 20.0f);
        }
    }
    CHECK(three == 1u);
    CHECK(one == 1u);
    CHECK(f.mgr->BuildCount() == 4u);

    // Unchanged instances: no rebuild. A brush stroke (a new instance) re-buckets the layer.
    sets = f.Extract(snapshot, nullptr);
    CHECK(f.mgr->BuildCount() == 4u);
    f.Prop().instances.PushBack(Float4x4::Translation(Float3{50.0f, 2.0f, -50.0f}));
    sets = f.Extract(snapshot, nullptr);
    CHECK(f.mgr->BuildCount() == 8u);
    CHECK(sets.Size() == 3u);
    // Erasing back to the old content is another hash: another re-bucket, two sets again.
    f.Prop().instances.RemoveAt(4);
    sets = f.Extract(snapshot, nullptr);
    CHECK(sets.Size() == 2u);
    // The fade prefix applies like any set: a far origin thins the props.
    const Float3 far{2000.0f, 5.0f, 0.0f};
    sets = f.Extract(snapshot, &far);
    CHECK(sets.IsEmpty());
}

TEST_CASE("engine.vegetation: an invalidated set keeps drawing its old instances until its rebuild lands; props rebuild whole")
{
    Fixture f(/*withSplat*/ false);
    f.mgr->SetBuildBudget(100);
    render::ExtractedScene snapshot{DefaultAllocator()};
    Array<const render::MultiMeshRenderData*> sets = f.Extract(snapshot, nullptr);
    REQUIRE(sets.Size() == 4u); // warm
    CHECK(f.mgr->BuildCount() == 4u);

    // A sculpt (every chunk dirty) under a budget of one: every frame still draws all four
    // sets - the three not yet rebuilt show their previous instances - while one rebuilds
    // per extraction, so nothing blinks under the brush.
    f.mgr->SetBuildBudget(1);
    f.grid->BumpVersion();
    for (u32 frame = 1; frame <= 4; ++frame)
    {
        sets = f.Extract(snapshot, nullptr);
        CHECK(sets.Size() == 4u);
        CHECK(f.mgr->BuildCount() == 4u + frame);
    }
    sets = f.Extract(snapshot, nullptr);
    CHECK(f.mgr->BuildCount() == 8u); // all caught up
    // A cold set (never built) still waits its turn: a fresh layer under the same budget.
    ProceduralVegetationLayer flowers = f.Layer();
    flowers.name = String(u8"Flowers");
    f.Component().proceduralLayers.PushBack(flowers);
    sets = f.Extract(snapshot, nullptr);
    CHECK(sets.Size() == 5u); // the four grass sets + the one flower chunk built this frame

    // Authored props re-bucket outside the budget: a stroke lands whole in one extraction.
    f.Component().proceduralLayers.RemoveAt(1);
    PropVegetationLayer& rocks = f.MakePropsOnly();
    for (i32 i = 0; i < 4; ++i)
    {
        rocks.instances.PushBack(Float4x4::Translation(
            Float3{(i % 2 == 0) ? -30.0f : 30.0f, 2.0f, (i < 2) ? -30.0f : 30.0f}));
    }
    sets = f.Extract(snapshot, nullptr); // a new layer: 4 builds
    REQUIRE(sets.Size() == 4u);
    const u64 builds = f.mgr->BuildCount();
    f.Prop().instances.PushBack(Float4x4::Translation(Float3{-31.0f, 2.0f, -31.0f}));
    sets = f.Extract(snapshot, nullptr);
    CHECK(sets.Size() == 4u);
    CHECK(f.mgr->BuildCount() == builds + 4u); // every chunk re-bucketed this frame, budget 1
    u32 total = 0;
    for (const render::MultiMeshRenderData* s : sets)
    {
        total += s->instanceCount;
    }
    CHECK(total == 5u);
}

TEST_CASE("engine.vegetation: a freshly added layer with a mesh grows nothing until it is painted or given a source")
{
    Fixture f(/*withSplat*/ true); // the splat has palette 0 painted: a Splat default would grow
    f.mgr->SetBuildBudget(100);
    ProceduralVegetationLayer fresh; // exactly what the inspector's add button makes, plus a mesh
    fresh.mesh = f.mesh.Get();
    f.Component().proceduralLayers.PushBack(fresh); // Mask placement, and no mask: nothing
    PropVegetationLayer props;                       // and a fresh prop layer holds nothing
    props.mesh = f.mesh.Get();
    f.Component().propLayers.PushBack(props);
    render::ExtractedScene snapshot{DefaultAllocator()};
    Array<const render::MultiMeshRenderData*> sets = f.Extract(snapshot, nullptr);
    for (const render::MultiMeshRenderData* s : sets)
    {
        CHECK(render::EntityTag::Index(s->entityId) == f.terrain.index);
    }
    CHECK(sets.Size() == 2u); // only the fixture's grass (the two painted chunks); nothing new
    // Painting one prop into the prop layer draws it; giving the procedural one the splat as
    // its source makes it grow like the grass.
    f.Prop().instances.PushBack(Float4x4::Translation(Float3{10.0f, 2.0f, 10.0f}));
    sets = f.Extract(snapshot, nullptr);
    CHECK(sets.Size() == 3u);
    f.Prop().instances.Clear();
    f.Layer(1).placement = veg::VegetationPlacement::Splat;
    sets = f.Extract(snapshot, nullptr);
    CHECK(sets.Size() == 4u);
}

TEST_CASE("engine.vegetation: cutting a hole after the first growth regrows the layer with nothing inside the cut")
{
    // The editor's hole brush: CutHoles bumps the heightfield version and every set regrows
    // (no InvalidateRegion), and the regrown scatter must leave the cut cells bare.
    bool splat = false; // Uniform, then the editor's Splat-placed grass
    SUBCASE("uniform") { splat = false; }
    SUBCASE("splat") { splat = true; }
    Fixture f(splat);
    const usize grown = splat ? 2u : 4u; // the splat half (x < 0) grows two of the four chunks
    render::ExtractedScene snapshot{DefaultAllocator()};
    Array<const render::MultiMeshRenderData*> sets = f.Extract(snapshot, nullptr);
    REQUIRE(sets.Size() == grown);
    const u32 before = TotalInstances(sets);
    REQUIRE(before > 0u);

    const auto countInside = [](const Array<const render::MultiMeshRenderData*>& s, f32 radius)
    {
        u32 n = 0;
        for (const render::MultiMeshRenderData* set : s)
        {
            for (u32 i = 0; i < set->instanceCount; ++i)
            {
                const f32 x = set->transforms[i].m[3][0];
                const f32 z = set->transforms[i].m[3][2];
                if (x * x + z * z < radius * radius)
                {
                    ++n;
                }
            }
        }
        return n;
    };
    // Grass grew before the cut: the splat half is x < 0, so probe the centre's left side.
    const auto countInsideLeft = [&](const Array<const render::MultiMeshRenderData*>& s, f32 radius)
    {
        u32 n = 0;
        for (const render::MultiMeshRenderData* set : s)
        {
            for (u32 i = 0; i < set->instanceCount; ++i)
            {
                const f32 x = set->transforms[i].m[3][0];
                const f32 z = set->transforms[i].m[3][2];
                if (x < 0.0f && x * x + z * z < radius * radius)
                {
                    ++n;
                }
            }
        }
        return n;
    };
    REQUIRE(countInsideLeft(sets, 10.0f) > 0u);

    (void)hf::CutHoles(*f.grid, 0.0f, 0.0f, 12.0f); // a 24 m cut at the centre, all four chunks
    REQUIRE(f.grid->HasHoles());
    sets = f.Extract(snapshot, nullptr);
    REQUIRE(sets.Size() == grown);
    CHECK(f.mgr->BuildCount() == 8u); // every set regrew
    CHECK(countInside(sets, 10.0f) == 0u); // nothing inside the cut (a 2 m margin to the rim)
    CHECK(TotalInstances(sets) < before);
}

TEST_CASE("engine.vegetation: a stamped prop over a cut cell does not draw and comes back with the fill")
{
    // The authored list is untouched (the eraser can still reach the prop through the hole);
    // the bucketed set simply leaves it out while its cell is cut.
    Fixture f(/*withSplat*/ false);
    f.mgr->SetBuildBudget(100);
    PropVegetationLayer& rocks = f.MakePropsOnly();
    rocks.instances.PushBack(Float4x4::Translation(Float3{-40.0f, 2.0f, -40.0f}));
    rocks.instances.PushBack(Float4x4::Translation(Float3{-20.0f, 2.0f, -20.0f}));
    rocks.instances.PushBack(Float4x4::Translation(Float3{20.0f, 2.0f, 20.0f}));

    render::ExtractedScene snapshot{DefaultAllocator()};
    Array<const render::MultiMeshRenderData*> sets = f.Extract(snapshot, nullptr);
    CHECK(TotalInstances(sets) == 3u);

    (void)hf::CutHoles(*f.grid, -20.0f, -20.0f, 3.0f); // under the second prop only
    sets = f.Extract(snapshot, nullptr);
    CHECK(TotalInstances(sets) == 2u);
    CHECK(f.Prop().instances.Size() == 3u); // the data keeps it
    for (const render::MultiMeshRenderData* s : sets)
    {
        for (u32 i = 0; i < s->instanceCount; ++i)
        {
            CHECK(s->transforms[i].m[3][0] != -20.0f);
        }
    }

    (void)hf::FillHoles(*f.grid, -20.0f, -20.0f, 3.0f);
    sets = f.Extract(snapshot, nullptr);
    CHECK(TotalInstances(sets) == 3u);
}

// ---- vegetation colliders (Specs/vegetation-colliders.md) ----

namespace
{
    // A procedural layer in the data-version-3 layout (a material list, no collision), as a scene
    // saved before 2026-10-06 stored it.
    void WriteLayerV3(ISerializer& ar)
    {
        String name(u8"Pines");
        foundation::resource::Ref<geometry::StaticMesh> mesh;
        mesh.SetId(Guid{0x11u, 0x22u});
        Array<foundation::resource::Ref<foundation::materials::Material>> materials;
        Float2 scaleRange{0.8f, 1.2f};
        f32 maxSlopeDegrees = 35.0f;
        Float2 heightRange{-1.0e6f, 1.0e6f};
        bool alignToNormal = false;
        f32 fadeStart = 40.0f;
        f32 fadeEnd = 80.0f;
        bool castShadows = true;
        u32 maxInstancesPerChunk = 4096;
        bool visible = true;
        u8 placement = 1; // Splat
        u32 splatLayer = 2;
        f32 splatThreshold = 0.5f;
        u32 maskPlane = 0;
        f32 density = 0.025f;
        foundation::core::Serialize(ar, "name", name);
        foundation::core::Serialize(ar, "mesh", mesh);
        foundation::core::Serialize(ar, "materials", materials);
        foundation::core::Serialize(ar, "scaleRange", scaleRange);
        foundation::core::Serialize(ar, "maxSlopeDegrees", maxSlopeDegrees);
        foundation::core::Serialize(ar, "heightRange", heightRange);
        foundation::core::Serialize(ar, "alignToNormal", alignToNormal);
        foundation::core::Serialize(ar, "fadeStart", fadeStart);
        foundation::core::Serialize(ar, "fadeEnd", fadeEnd);
        foundation::core::Serialize(ar, "castShadows", castShadows);
        foundation::core::Serialize(ar, "maxInstancesPerChunk", maxInstancesPerChunk);
        foundation::core::Serialize(ar, "visible", visible);
        foundation::core::Serialize(ar, "placement", placement);
        foundation::core::Serialize(ar, "splatLayer", splatLayer);
        foundation::core::Serialize(ar, "splatThreshold", splatThreshold);
        foundation::core::Serialize(ar, "maskPlane", maskPlane);
        foundation::core::Serialize(ar, "density", density);
    }

    Array<scene::StaticCapsule> Capsules(Fixture& f, bool expectReady = true)
    {
        Array<scene::StaticCapsule> out;
        scene::IStaticColliderSource* source = f.mgr->AsStaticColliderSource();
        REQUIRE(source != nullptr);
        CHECK(source->CollectStaticCapsules(f.scene, out) == expectReady);
        return out;
    }
}

TEST_CASE("engine.vegetation: a data-version-3 layer reads as scenery only; version 4 round-trips its collision")
{
    engine::vegetation::RegisterVegetationComponentReflection();
    const TypeInfo& type = TypeOf<TerrainVegetationComponent>();
    MemoryStream stream;
    {
        BinarySerializer ar(stream, SerializeMode::Write);
        const SerializedDataVersion chain[] = {{type.id, 3u}};
        u32 n = 1;
        ar.Key("dataVersions");
        ar.BeginArray(n);
        SerializedDataVersion entry = chain[0];
        ar.Key("type");
        ar.Scalar(&entry.typeId, ScalarKind::UInt64);
        ar.Key("version");
        ar.Scalar(&entry.version, ScalarKind::UInt32);
        ar.EndArray();
        ar.PushVersionScope(chain, 1);
        u32 count = 1;
        ar.Key("proceduralLayers");
        ar.BeginArray(count);
        WriteLayerV3(ar);
        ar.EndArray();
        Array<PropVegetationLayer> none;
        foundation::core::Serialize(ar, "propLayers", none);
        foundation::resource::Ref<veg::VegetationMask> mask;
        foundation::core::Serialize(ar, "mask", mask);
        bool visible = true;
        foundation::core::Serialize(ar, "visible", visible);
        ar.PopVersionScope();
        REQUIRE(ar.IsOk());
    }
    REQUIRE(stream.Seek(0, SeekOrigin::Begin) == 0);
    TerrainVegetationComponent loaded;
    {
        BinarySerializer ar(stream, SerializeMode::Read);
        BeginVersionedPayload(ar, type);
        CHECK(ar.Version() == 3u);
        Serialize(ar, loaded);
        EndVersionedPayload(ar);
        REQUIRE(ar.IsOk());
    }
    REQUIRE(loaded.proceduralLayers.Size() == 1u);
    CHECK(loaded.proceduralLayers[0].collisionRadius == 0.0f);
    CHECK(loaded.proceduralLayers[0].collisionHeight == 0.0f);
    CHECK(loaded.proceduralLayers[0].density == 0.025f); // the fields after the base still line up

    // The current version writes and reads the collision.
    TerrainVegetationComponent solid;
    ProceduralVegetationLayer pines;
    pines.collisionRadius = 0.3f;
    pines.collisionHeight = 4.5f;
    pines.collisionGroup = 2;
    pines.density = 0.06f;
    solid.proceduralLayers.PushBack(pines);
    MemoryStream current;
    {
        BinarySerializer ar(current, SerializeMode::Write);
        BeginVersionedPayload(ar, type);
        Serialize(ar, solid);
        EndVersionedPayload(ar);
        REQUIRE(ar.IsOk());
    }
    REQUIRE(current.Seek(0, SeekOrigin::Begin) == 0);
    TerrainVegetationComponent back;
    {
        BinarySerializer ar(current, SerializeMode::Read);
        BeginVersionedPayload(ar, type);
        CHECK(ar.Version() == 4u);
        Serialize(ar, back);
        EndVersionedPayload(ar);
        REQUIRE(ar.IsOk());
    }
    REQUIRE(back.proceduralLayers.Size() == 1u);
    CHECK(back.proceduralLayers[0].collisionRadius == 0.3f);
    CHECK(back.proceduralLayers[0].collisionHeight == 4.5f);
    CHECK(back.proceduralLayers[0].collisionGroup == 2u);
    CHECK(back.proceduralLayers[0].density == 0.06f);
}

TEST_CASE("engine.vegetation: a solid prop layer is a capsule per instance, scaled, in its group; scenery is none")
{
    Fixture f(/*withSplat*/ false);
    PropVegetationLayer& rocks = f.MakePropsOnly();
    rocks.instances.PushBack(Float4x4::Translation(Float3{-40.0f, 2.0f, -40.0f}));
    rocks.instances.PushBack(Float4x4::Scale(Float3{2.0f, 2.0f, 2.0f}) *
                             Float4x4::Translation(Float3{20.0f, 2.0f, 20.0f}));
    CHECK(Capsules(f).IsEmpty()); // radius 0: scenery only

    f.Prop().collisionRadius = 0.25f;
    f.Prop().collisionHeight = 3.0f;
    f.Prop().collisionGroup = 5;
    Array<scene::StaticCapsule> capsules = Capsules(f);
    REQUIRE(capsules.Size() == 2u);
    bool small = false, large = false;
    for (const scene::StaticCapsule& c : capsules)
    {
        CHECK(c.group == 5u);
        CHECK(c.foot.y == doctest::Approx(2.0f));
        if (c.foot.x < 0.0f)
        {
            small = true;
            CHECK(c.foot.x == doctest::Approx(-40.0f));
            CHECK(c.radius == doctest::Approx(0.25f));
            CHECK(c.height == doctest::Approx(3.0f));
        }
        else
        {
            large = true; // the 2x instance: a 2x trunk
            CHECK(c.foot.x == doctest::Approx(20.0f));
            CHECK(c.radius == doctest::Approx(0.5f));
            CHECK(c.height == doctest::Approx(6.0f));
        }
    }
    CHECK(small);
    CHECK(large);

    // The terrain entity moved: the trunks stand where the drawn instances do.
    f.scene.SetLocalTransform(f.terrain, Transform{Float3{100.0f, 0.0f, 0.0f}, Quaternion::Identity,
                                                   Float3{1.0f, 1.0f, 1.0f}});
    f.scene.UpdateTransforms();
    capsules = Capsules(f);
    REQUIRE(capsules.Size() == 2u);
    for (const scene::StaticCapsule& c : capsules)
    {
        CHECK(c.foot.x > 50.0f);
    }
}

TEST_CASE("engine.vegetation: a solid procedural layer's capsules are the drawn instances over the whole terrain")
{
    Fixture f(/*withSplat*/ false); // Uniform: every chunk grows
    f.Layer().collisionRadius = 0.2f;
    f.Layer().collisionHeight = 2.0f;
    f.Layer().scaleRange = Float2{1.0f, 1.0f};
    Array<scene::StaticCapsule> capsules = Capsules(f);
    CHECK(!capsules.IsEmpty());

    // Headless, the drawing takes every chunk: the same instances, the same places.
    f.mgr->SetBuildBudget(100);
    render::ExtractedScene snapshot{DefaultAllocator()};
    Array<const render::MultiMeshRenderData*> sets = f.Extract(snapshot, nullptr);
    u32 drawn = 0;
    for (const render::MultiMeshRenderData* s : sets)
    {
        drawn += s->uploadCount;
    }
    REQUIRE(drawn == capsules.Size());
    usize matched = 0;
    for (const render::MultiMeshRenderData* s : sets)
    {
        for (u32 i = 0; i < s->uploadCount; ++i)
        {
            const Float3 p{s->transforms[i].m[3][0], s->transforms[i].m[3][1], s->transforms[i].m[3][2]};
            for (const scene::StaticCapsule& c : capsules)
            {
                if (Length(c.foot - p) < 1e-4f)
                {
                    ++matched;
                    break;
                }
            }
        }
    }
    CHECK(matched == capsules.Size());

    // A hidden layer or component is still solid (a view toggle); an inactive terrain is not.
    f.Layer().visible = false;
    f.Component().visible = false;
    CHECK(Capsules(f).Size() == capsules.Size());
    f.scene.SetActive(f.terrain, false);
    CHECK(Capsules(f).IsEmpty());
}

TEST_CASE("engine.vegetation: a solid layer whose mesh is still resolving is not ready, and adds nothing")
{
    Fixture f(/*withSplat*/ false);
    f.Layer().collisionRadius = 0.2f;
    f.Layer().collisionHeight = 2.0f;
    f.Layer().mesh = nullptr;
    f.Layer().mesh.SetId(Guid{0x51u, 0x52u}); // named, not bound yet
    CHECK(Capsules(f, /*expectReady*/ false).IsEmpty());
    f.Layer().mesh = f.mesh.Get(); // it resolved
    CHECK(!Capsules(f).IsEmpty());
}
