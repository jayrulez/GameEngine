// Resource-reference layer (asset-pipeline 6b): MeshComponent's resource::Ref fields
// round-trip through scene serialization by Guid and resolve through the ResourceManager's
// PROXY HANDLES - so a Reload swaps the product behind every holder without a re-resolve.

#include <doctest/doctest.h>

#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

import draconic.foundation;
import draconic.vfs;
import draconic.content;
import draconic.resource;
import draconic.geometry;
import draconic.geometry.resource;
import draconic.materials;
import draconic.materials.resource;
import draconic.scene;
import draconic.scene.resource;
import draconic.engine.render;

using namespace draconic::foundation;
using namespace draconic::render;
namespace scene = draconic::scene;
namespace resource = draconic::resource;
namespace geometry = draconic::geometry;

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

TEST_CASE("resource-ref: scene round-trip resolves mesh refs through proxy handles")
{
    const StringView dir = u8"draconic_resref_test_db";
    RemoveTree(dir);
    (void)CreateDirectory(dir);
    draconic::vfs::NativeFileSystem mount(dir);

    // A cooked mesh product: a unit cube baked into a StaticMeshSource instance.
    GlobalTypeRegistry().Register(geometry::StaticMeshSource::StaticType());
    RegisterSerializable<geometry::StaticMeshSource>();
    draconic::content::ContentDatabase cookedDb(mount, BinarySerializerFactory(), u8".rasset");
    Guid meshId;
    {
        RefPtr<geometry::StaticMesh> cube = geometry::Primitives::Cube(1.0f);
        geometry::StaticMeshSource source;
        geometry::StaticMeshSource::FromMesh(*cube, source);
        draconic::content::Instance* inst = cookedDb.RootGroup()->CreateInstance(
            u8"Cube", geometry::StaticMeshSource::StaticType());
        REQUIRE(inst != nullptr);
        REQUIRE(inst->WriteObject(source).IsOk());
        meshId = inst->Id();
    }

    resource::ResourceManager resources(cookedDb);
    geometry::StaticMeshFactory meshFactory;
    resources.AddFactory(&meshFactory);

    // Author a scene whose MeshComponent references the mesh BY GUID only.
    MemoryStream blob;
    {
        scene::Scene scene;
        scene.AddSystem<MeshComponentManager>();
        const scene::EntityHandle e = scene.CreateEntity(u8"Box");
        MeshComponent& mc = scene.GetSystem<MeshComponentManager>()->Add(e);
        mc.mesh.SetId(meshId);
        mc.color = Color{0.5f, 0.25f, 0.125f, 1.0f};

        BinarySerializer ar(blob, SerializeMode::Write);
        scene::SerializeScene(ar, scene);
        REQUIRE(ar.IsOk());
    }

    // Load into a FRESH scene, then run the post-load resolve pass.
    scene::Scene loaded;
    loaded.AddSystem<MeshComponentManager>();
    REQUIRE(blob.Seek(0, SeekOrigin::Begin) == 0);
    {
        BinarySerializer ar(blob, SerializeMode::Read);
        scene::SerializeScene(ar, loaded);
        REQUIRE(ar.IsOk());
    }

    auto* meshes = loaded.GetSystem<MeshComponentManager>();
    REQUIRE(meshes != nullptr);
    REQUIRE(meshes->ComponentCount() == 1u);
    MeshComponent* mc = nullptr;
    meshes->ForEach([&](MeshComponent& c, scene::EntityHandle) { mc = &c; });
    REQUIRE(mc != nullptr);

    // Identity survived; the product isn't bound until the resolve pass runs.
    CHECK(mc->mesh.id == meshId);
    CHECK(mc->mesh.Get() == nullptr);
    CHECK(mc->color.r == doctest::Approx(0.5f));

    scene::ResolveSceneResources(loaded, resources);
    geometry::StaticMesh* live = mc->mesh.Get();
    REQUIRE(live != nullptr);
    CHECK(live->vertices.Size() > 0u);
    const f32 sizeBefore = live->bounds.max.x - live->bounds.min.x;
    CHECK(sizeBefore == doctest::Approx(1.0f));

    // PROXY semantics: rewrite the cooked source (a 2x cube), Reload, and the SAME ref sees
    // the new product through its handle - no re-resolve pass. (Pointer equality is not a
    // valid signal: the allocator may reuse the freed block.)
    {
        RefPtr<geometry::StaticMesh> bigger = geometry::Primitives::Cube(2.0f);
        geometry::StaticMeshSource source;
        geometry::StaticMeshSource::FromMesh(*bigger, source);
        REQUIRE(cookedDb.GetInstance(meshId)->WriteObject(source).IsOk());
    }
    REQUIRE(resources.Reload(meshId));
    geometry::StaticMesh* reloaded = mc->mesh.Get();
    REQUIRE(reloaded != nullptr);
    const f32 sizeAfter = reloaded->bounds.max.x - reloaded->bounds.min.x;
    CHECK(sizeAfter == doctest::Approx(2.0f));

    RemoveTree(dir);
}

TEST_CASE("resource-ref: direct objects win over proxies and skip serialization")
{
    RefPtr<geometry::StaticMesh> procedural = geometry::Primitives::Cube(2.0f);

    MeshComponent mc;
    mc.mesh = procedural; // the sample/procedural path: plain RefPtr assignment
    CHECK(mc.mesh.Get() == procedural.Get());
    CHECK(mc.mesh.id.IsNil()); // nothing to serialize - direct objects are runtime-only

    // A guid alongside a direct object: the direct object still wins at Get().
    Random rng(1234);
    mc.mesh.SetId(Guid::Generate(rng));
    CHECK(mc.mesh.Get() == procedural.Get());
}

TEST_CASE("resource-ref: sprite + decal texture refs round-trip by guid")
{
    // No GPU here: the ids must survive scene serialization through the serializable managers;
    // resolving to a live texture::Texture is covered by the editor's TextureFactory path.
    Random rng(42);
    const Guid spriteTex = Guid::Generate(rng);
    const Guid decalTex = Guid::Generate(rng);

    MemoryStream blob;
    {
        scene::Scene scene;
        scene.AddSystem<SpriteComponentManager>();
        scene.AddSystem<DecalComponentManager>();
        const scene::EntityHandle e = scene.CreateEntity(u8"Deco");
        SpriteComponent& sc = scene.GetSystem<SpriteComponentManager>()->Add(e);
        sc.textureAsset.SetId(spriteTex);
        sc.size = Float2{2.0f, 3.0f};
        sc.additive = true;
        DecalComponent& dc = scene.GetSystem<DecalComponentManager>()->Add(e);
        dc.textureAsset.SetId(decalTex);
        dc.fadeEnd = 0.5f;

        BinarySerializer ar(blob, SerializeMode::Write);
        scene::SerializeScene(ar, scene);
        REQUIRE(ar.IsOk());
    }

    scene::Scene loaded;
    loaded.AddSystem<SpriteComponentManager>();
    loaded.AddSystem<DecalComponentManager>();
    REQUIRE(blob.Seek(0, SeekOrigin::Begin) == 0);
    {
        BinarySerializer ar(blob, SerializeMode::Read);
        scene::SerializeScene(ar, loaded);
        REQUIRE(ar.IsOk());
    }

    SpriteComponent* sc = nullptr;
    loaded.GetSystem<SpriteComponentManager>()->ForEach([&](SpriteComponent& c, scene::EntityHandle)
                                                        { sc = &c; });
    REQUIRE(sc != nullptr);
    CHECK(sc->textureAsset.id == spriteTex);
    CHECK(sc->size.x == doctest::Approx(2.0f));
    CHECK(sc->additive);
    CHECK(sc->texture == nullptr); // the raw view override is runtime-only

    DecalComponent* dc = nullptr;
    loaded.GetSystem<DecalComponentManager>()->ForEach([&](DecalComponent& c, scene::EntityHandle)
                                                       { dc = &c; });
    REQUIRE(dc != nullptr);
    CHECK(dc->textureAsset.id == decalTex);
    CHECK(dc->fadeEnd == doctest::Approx(0.5f));
}

TEST_CASE("resource-ref: instanced-mesh refs + authored placement round-trip")
{
    Random rng(7);
    const Guid meshId = Guid::Generate(rng);
    const Guid matId = Guid::Generate(rng);

    MemoryStream blob;
    {
        scene::Scene scene;
        scene.AddSystem<InstancedMeshComponentManager>();
        const scene::EntityHandle e = scene.CreateEntity(u8"Scatter");
        InstancedMeshComponent& c = scene.GetSystem<InstancedMeshComponentManager>()->Add(e);
        c.mesh.SetId(meshId);
        c.material.SetId(matId);
        // Replace the editor-workflow seed (a fresh component starts with ONE identity instance).
        REQUIRE(c.Count() == 1u);
        const Float4x4 xf[2] = {Float4x4::Translation(Float3{1, 0, 0}),
                                Float4x4::Translation(Float3{0, 2, 0})};
        c.SetInstances(Span<const Float4x4>{xf, 2});
        c.tints.PushBack(Color{1, 0, 0, 1});
        c.tints.PushBack(Color{0, 1, 0, 1});

        BinarySerializer ar(blob, SerializeMode::Write);
        scene::SerializeScene(ar, scene);
        REQUIRE(ar.IsOk());
    }

    scene::Scene loaded;
    loaded.AddSystem<InstancedMeshComponentManager>();
    REQUIRE(blob.Seek(0, SeekOrigin::Begin) == 0);
    {
        BinarySerializer ar(blob, SerializeMode::Read);
        scene::SerializeScene(ar, loaded);
        REQUIRE(ar.IsOk());
    }

    InstancedMeshComponent* c = nullptr;
    loaded.GetSystem<InstancedMeshComponentManager>()->ForEach(
        [&](InstancedMeshComponent& ic, scene::EntityHandle) { c = &ic; });
    REQUIRE(c != nullptr);
    CHECK(c->mesh.id == meshId);
    CHECK(c->material.id == matId);
    REQUIRE(c->Count() == 2u);
    CHECK(c->instances[0].m[3][0] == doctest::Approx(1.0f));
    CHECK(c->instances[1].m[3][1] == doctest::Approx(2.0f));
    REQUIRE(c->tints.Size() == 2u);
    CHECK(c->tints[1].g == doctest::Approx(1.0f));
    // Runtime state starts fresh: version 1 vs boundsVersion 0 => first extract re-uploads.
    CHECK(c->version >= 1u);
    CHECK(c->boundsVersion == 0u);
    CHECK(c->posePool == nullptr);
}

TEST_CASE("resource-ref: a Ref<StaticMesh> bound to a SKINNED product keeps the skin stream")
{
    // The editor's mesh picker offers SkinnedMeshAssets for MeshComponent's Ref<StaticMesh>
    // (SkinnedMesh IS-A StaticMesh). The StaticMeshFactory must build the REAL SkinnedMesh for
    // a skinned product - building it as a plain StaticMesh silently drops the skin stream and
    // the mesh can never animate (the "fox plays but doesn't move" bug).
    const StringView dir = u8"draconic_skinref_test_db";
    RemoveTree(dir);
    (void)CreateDirectory(dir);
    draconic::vfs::NativeFileSystem mount(dir);

    GlobalTypeRegistry().Register(geometry::SkinnedMeshSource::StaticType());
    RegisterSerializable<geometry::SkinnedMeshSource>();
    draconic::content::ContentDatabase cookedDb(mount, BinarySerializerFactory(), u8".rasset");

    Guid meshId;
    {
        // A skinned cube: the static cube's streams + one skinning entry per vertex.
        RefPtr<geometry::StaticMesh> cube = geometry::Primitives::Cube(1.0f);
        RefPtr<geometry::SkinnedMesh> skinned = MakeRef<geometry::SkinnedMesh>(DefaultAllocator());
        skinned->vertices = cube->vertices;
        skinned->indices = cube->indices;
        skinned->subMeshes = cube->subMeshes;
        skinned->bounds = cube->bounds;
        for (usize i = 0; i < skinned->vertices.Size(); ++i)
        {
            skinned->skinning.PushBack(geometry::VertexSkinning{});
        }
        skinned->skeletonIndex = 0;

        geometry::SkinnedMeshSource source;
        geometry::SkinnedMeshSource::FromMesh(*skinned, source);
        draconic::content::Instance* inst = cookedDb.RootGroup()->CreateInstance(
            u8"SkinnedCube", geometry::SkinnedMeshSource::StaticType());
        REQUIRE(inst != nullptr);
        REQUIRE(inst->WriteObject(source).IsOk());
        meshId = inst->Id();
    }

    resource::ResourceManager resources(cookedDb);
    geometry::StaticMeshFactory meshFactory;
    resources.AddFactory(&meshFactory);

    MeshComponent mc;
    mc.mesh.SetId(meshId);
    mc.mesh.Bind(resources);
    geometry::StaticMesh* live = mc.mesh.Get();
    REQUIRE(live != nullptr);
    CHECK(live->IsSkinned());
    CHECK(live->vertices.Size() == 24u);
    auto* skinnedLive = Cast<geometry::SkinnedMesh>(live);
    REQUIRE(skinnedLive != nullptr);
    CHECK(skinnedLive->SkinningStream().Size() == 24u);

    RemoveTree(dir);
}

TEST_CASE("resource-ref: per-submesh material refs round-trip and resolve to the RefPtr array")
{
    // Serialized identity (submeshMaterialRefs) -> resolve pass -> the raw RefPtr array
    // extraction reads. Binding itself is Ref::Bind (covered by the mesh tests above);
    // real Material products would drag the shader stack in, so the manager here has no
    // material factory and the materialized entries stay null - the SHAPE is what's under
    // test: ids round-trip, the array materializes on resolve, direct fills survive.
    RegisterRenderComponentReflection(); // patches TypeOf<MeshComponent>().dataVersion (v2 gate)
    const StringView dir = u8"draconic_submesh_ref_test_db";
    RemoveTree(dir);
    (void)CreateDirectory(dir);
    draconic::vfs::NativeFileSystem mount(dir);
    draconic::content::ContentDatabase cookedDb(mount, BinarySerializerFactory(), u8".rasset");
    resource::ResourceManager resources(cookedDb);

    const Guid matA{0xA1, 0x1};
    const Guid matB{0xB2, 0x2};
    MemoryStream blob;
    {
        scene::Scene scene;
        scene.AddSystem<MeshComponentManager>();
        const scene::EntityHandle e = scene.CreateEntity(u8"Multi");
        MeshComponent& mc = scene.GetSystem<MeshComponentManager>()->Add(e);
        draconic::resource::Ref<draconic::materials::Material> ra, rb;
        ra.SetId(matA);
        rb.SetId(matB);
        mc.materials.PushBack(ra);
        mc.materials.PushBack(rb);

        BinarySerializer ar(blob, SerializeMode::Write);
        scene::SerializeScene(ar, scene);
        REQUIRE(ar.IsOk());
    }

    scene::Scene loaded;
    loaded.AddSystem<MeshComponentManager>();
    REQUIRE(blob.Seek(0, SeekOrigin::Begin) == 0);
    {
        BinarySerializer ar(blob, SerializeMode::Read);
        scene::SerializeScene(ar, loaded);
        REQUIRE(ar.IsOk());
    }
    MeshComponent* mc = nullptr;
    loaded.GetSystem<MeshComponentManager>()->ForEach([&](MeshComponent& c, scene::EntityHandle)
                                                      { mc = &c; });
    REQUIRE(mc != nullptr);
    REQUIRE(mc->materials.Size() == 2u);
    CHECK(mc->materials[0].id == matA);
    CHECK(mc->materials[1].id == matB);

    scene::ResolveSceneResources(loaded, resources); // attaches bindings (cache fills at extract)
    CHECK(mc->materials.Size() == 2u);

    RemoveTree(dir);
}
