// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Pipeline::ModelImporter tests - load a real glTF, cook it through the importer into a
// content DB, then bind the cooked ModelResource back through the resource manager and
// verify the whole chain from convert through cook to bind (manifest nodes and resolved meshes).

#include "Core/Prelude.h"
#include <doctest/doctest.h>
#include <cmath>
#include <initializer_list>

import foundation.core;
import foundation.vfs;
import foundation.content;
import foundation.resource;
import foundation.geometry;
import foundation.geometry.resource;
import foundation.animation;
import foundation.animation.resource;
import foundation.model;
import foundation.model.io;
import foundation.model.gltf;
import foundation.image;
import foundation.image.dds;
import modelimporter;
import physics.pipeline;
import pipeline.core;
import editor.core;
import pipeline.importer;
import pipeline.cook;

using namespace pipeline;
import texture.pipeline;
import texture.compression;
import geometry.pipeline;
import materials.pipeline;
import animation.pipeline;
import foundation.materials;
import foundation.texture;
import foundation.texture.resource;
import foundation.rhi;
import foundation.rhi.null;
import foundation.materials.resource;

using namespace foundation::core;
namespace vfs = foundation::vfs;
namespace content = foundation::content;
namespace resource = foundation::resource;
namespace geometry = foundation::geometry;
namespace model = foundation::model;

namespace
{
    // The sample models under the data root, found the way every executable finds engine data.
    String MiPath(StringView relative)
    {
        static const String root = vfs::FindDataRoot();
        REQUIRE_FALSE(root.IsEmpty());
        return vfs::DataPath(root.AsView(), relative);
    }
    String MiDuck() { return MiPath(u8"Samples/Models/Duck/glTF/Duck.gltf"); }
    String MiFox() { return MiPath(u8"Samples/Models/Fox/glTF/Fox.gltf"); }
    String MiFoxNested() { return MiPath(u8"Tests/Models/FoxNested/glTF/Fox.gltf"); }
    String MiGlb()
    {
        return MiPath(u8"Tests/Models/KenneyPlatformerCharacters/GLB/character-oozi.glb");
    }
}

TEST_CASE("import glTF -> cooked ModelResource round-trips through the resource system")
{
    const String duckStorage = MiDuck();
    const StringView duck = duckStorage.AsView();
    if (duck.IsEmpty())
    {
        return;
    } // path not configured (skip)

    model::RegisterModelResourceTypes(); // make the cooked types deserializable

    vfs::NativeFileSystem mount(u8"scratch_modelimporter_test_db", DefaultAllocator());
    content::ContentDatabase db(foundation::core::DefaultAllocator(), mount, foundation::core::BinarySerializerFactory(), u8".rasset");

    // Cook the model file into the DB; get back the manifest (ModelResource) Guid.
    Guid modelGuid;
    const model::ModelLoadResult r = pipeline::LoadAndCook(duck, db, u8"Duck", modelGuid);
    REQUIRE(r == model::ModelLoadResult::Ok);
    REQUIRE_FALSE(modelGuid.IsNil());

    // Bind the composite model: ModelFactory resolves its meshes via StaticMeshFactory.
    resource::ResourceManager manager(DefaultAllocator(), db);
    geometry::StaticMeshFactory meshFactory(DefaultAllocator());
    model::ModelFactory modelFactory;
    manager.AddFactory(&meshFactory);
    manager.AddFactory(&modelFactory);

    resource::Proxy<model::ModelResource> model = manager.Bind<model::ModelResource>(modelGuid);
    REQUIRE(model);
    CHECK(model->nodes.Size() > 0);
    CHECK(model->meshes.Size() > 0);

    // At least one node references a mesh, and that mesh resolved with real geometry.
    bool sawMesh = false;
    for (const pipeline::ModelNode& n : model->nodes)
    {
        if (n.meshIndex >= 0 && static_cast<usize>(n.meshIndex) < model->meshes.Size())
        {
            geometry::StaticMesh* mesh = model->meshes[static_cast<usize>(n.meshIndex)].Get();
            REQUIRE(mesh != nullptr);
            CHECK(mesh->VertexCount() > 0);
            CHECK(mesh->IndexCount() > 0);
            sawMesh = true;
        }
    }
    CHECK(sawMesh);
}

TEST_CASE("import skinned glTF -> cooked skeleton + animations + skinned mesh")
{
    const String foxStorage = MiFox();
    const StringView fox = foxStorage.AsView();
    if (fox.IsEmpty())
    {
        return;
    }

    model::RegisterModelResourceTypes();

    vfs::NativeFileSystem mount(u8"scratch_modelimporter_fox_db", DefaultAllocator());
    content::ContentDatabase db(foundation::core::DefaultAllocator(), mount, foundation::core::BinarySerializerFactory(), u8".rasset");

    Guid modelGuid;
    REQUIRE(pipeline::LoadAndCook(fox, db, u8"Fox", modelGuid) == model::ModelLoadResult::Ok);

    resource::ResourceManager manager(DefaultAllocator(), db);
    geometry::StaticMeshFactory meshFactory(DefaultAllocator());
    geometry::SkinnedMeshFactory skinnedFactory(DefaultAllocator());
    model::ModelFactory modelFactory;
    foundation::animation::SkeletonFactory skeletonFactory(DefaultAllocator());
    foundation::animation::AnimationClipFactory clipFactory(DefaultAllocator());
    manager.AddFactory(&meshFactory);
    manager.AddFactory(&skinnedFactory);
    manager.AddFactory(&modelFactory);
    manager.AddFactory(&skeletonFactory);
    manager.AddFactory(&clipFactory);

    resource::Proxy<model::ModelResource> model = manager.Bind<model::ModelResource>(modelGuid);
    REQUIRE(model);

    // The Fox is skinned and animated: a resolved skeleton with bones, and animation clips.
    REQUIRE(model->skeleton);
    CHECK(model->skeleton->BoneCount() > 0);
    CHECK(model->animations.Size() > 0);
    for (const auto& clip : model->animations)
    {
        REQUIRE(clip);
        CHECK(clip->duration > 0.0f);
    }

    // The mesh is flagged skinned and resolved as a SkinnedMesh (skin stream present).
    REQUIRE(model->meshes.Size() > 0);
    bool anySkinned = false;
    for (usize i = 0; i < model->meshSkinned.Size(); ++i)
    {
        if (model->meshSkinned[i] != 0)
        {
            anySkinned = true;
            geometry::StaticMesh* mesh = model->meshes[i].Get();
            REQUIRE(mesh != nullptr);
            CHECK(mesh->IsSkinned());
        }
    }
    CHECK(anySkinned);
}

// === Source-side file import (editor pipeline): GLB fan-out and full incremental cook ===

TEST_CASE("model-import: GLB fans out into source assets and cooks through the driver")
{
    using namespace editor;
    using namespace pipeline;

    // Types the fan-out creates, and their builders.
    pipeline::RegisterModelManifestAsset();
    pipeline::RegisterTextureAsset();
    pipeline::RegisterMeshAssets();
    pipeline::RegisterMaterialAsset();
    pipeline::RegisterAnimationAssets();
    // Product types too (the test reads a cooked product back).
    GlobalTypeRegistry().Register(foundation::geometry::StaticMeshSource::StaticType());
    GlobalTypeRegistry().Register(foundation::geometry::SkinnedMeshSource::StaticType());
    RegisterSerializable<foundation::geometry::StaticMeshSource>();
    RegisterSerializable<foundation::geometry::SkinnedMeshSource>();

    const StringView dir = u8"scratch_model_import_project";
    auto cleanTree = [&]()
    {
        // Recursive best-effort cleanup of the Content, Cooked, Sources and .cache trees.
        for (StringView sub : {u8"Content", u8"Cooked", u8"Sources", u8".cache"})
        {
            foundation::vfs::NativeFileSystem fs(PathJoin(dir, sub).AsView(), foundation::core::DefaultAllocator());
            Array<foundation::vfs::DirEntry> tops;
            if (fs.AsEnumerable()->Enumerate(u8"", tops).IsOk())
            {
                for (const auto& top : tops)
                {
                    if (!top.isDirectory)
                    {
                        (void)fs.AsWritable()->Delete(top.name.AsView());
                        continue;
                    }
                    Array<foundation::vfs::DirEntry> inner;
                    if (fs.AsEnumerable()->Enumerate(top.name.AsView(), inner).IsOk())
                    {
                        for (const auto& e : inner)
                        {
                            (void)fs.AsWritable()->Delete(
                                PathJoin(top.name.AsView(), e.name.AsView()).AsView());
                        }
                    }
                    (void)RemoveDirectory(
                        PathJoin(PathJoin(dir, sub).AsView(), top.name.AsView()).AsView());
                }
            }
            (void)RemoveDirectory(PathJoin(dir, sub).AsView());
        }
        FileDelete(PathJoin(dir, u8"Project.xml"));
        (void)RemoveDirectory(PathJoin(dir, u8"Editor"));
        (void)RemoveDirectory(dir);
    };
    cleanTree();
    REQUIRE(EditorProject::Create(DefaultAllocator(), dir, u8"P").IsOk());
    UniquePtr<EditorProject> project = EditorProject::Open(DefaultAllocator(), dir);
    REQUIRE(static_cast<bool>(project));

    // Import the Kenney character GLB (skinned: a skeleton and clips expected).
    pipeline::ModelFileImporter importer;
    CHECK(importer.Accepts(u8"glb"));
    Result<foundation::content::Instance*> imported =
        importer.Import(MiGlb().AsView(),
                        pipeline::ImportContext{DefaultAllocator(), project->SourcesRoot()}, *project->SourceDb().RootGroup(), nullptr, nullptr, nullptr);
    REQUIRE(imported.HasValue());
    foundation::content::Instance* manifestInst = imported.Value();
    REQUIRE(manifestInst != nullptr);
    CHECK(manifestInst->TypeName() == StringView(u8"ModelManifestAsset"));

    // The fan-out produces a subgroup: meshes and a manifest at minimum.
    foundation::content::Group* modelGroup =
        project->SourceDb().RootGroup()->GetGroup(u8"character-oozi");
    REQUIRE(modelGroup != nullptr);
    CHECK(modelGroup->Instances().Size() >= 2u);

    RefPtr<ISerializable> object = manifestInst->ReadObject();
    auto* manifestAsset = Cast<pipeline::ModelManifestAsset>(object.Get());
    REQUIRE(manifestAsset != nullptr);
    REQUIRE(manifestAsset->manifest.meshGuids.Size() >= 1u);
    CHECK(manifestAsset->manifest.nodes.Size() >= 1u);

    // Cook everything through the incremental driver (the real pipeline path).
    BuilderRegistry builders{DefaultAllocator()};
    auto add = [&](auto* builder)
    { builders.Register(UniquePtr<IAssetBuilder>(builder, DefaultAllocator())); };
    add(DefaultAllocator().New<pipeline::TextureAssetBuilder>());
    add(DefaultAllocator().New<pipeline::StaticMeshAssetBuilder>());
    add(DefaultAllocator().New<pipeline::SkinnedMeshAssetBuilder>());
    add(DefaultAllocator().New<pipeline::MaterialAssetBuilder>());
    add(DefaultAllocator().New<pipeline::SkeletonAssetBuilder>());
    add(DefaultAllocator().New<pipeline::AnimationClipAssetBuilder>());
    add(DefaultAllocator().New<pipeline::ModelManifestAssetBuilder>());

    foundation::vfs::NativeFileSystem sourcesFs(project->SourcesRoot().AsView(), foundation::core::DefaultAllocator());
    foundation::vfs::NativeFileSystem cacheFs(project->CacheRoot().AsView(), foundation::core::DefaultAllocator());
    CookDriver driver(DefaultAllocator(), project->SourceDb(), project->CookedDb(), builders, &sourcesFs, &cacheFs);

    CookPlan plan = driver.Plan();
    CHECK(plan.dirty.Size() >= 2u); // manifest and meshes (and any materials, skeleton, clips)
    CookStats stats = driver.Execute(plan);
    CHECK(stats.failed == 0u);
    CHECK(stats.cooked == plan.dirty.Size());
    CHECK(driver.Plan().dirty.IsEmpty()); // incremental: everything clean now

    // Products land under the source guids: the manifest product and a bindable mesh.
    CHECK(project->CookedDb().GetInstance(manifestInst->Id()) != nullptr);
    const Guid meshGuid = manifestAsset->manifest.meshGuids[0];
    RefPtr<ISerializable> meshProduct = project->CookedDb().ReadObject(meshGuid);
    REQUIRE(meshProduct.Get() != nullptr);

    cleanTree();
}

// A .gltf with external sidecars (Fox.bin, Texture.png) imports: the importer loads from the
// original path, where the sidecars are, and copies the referenced sidecars into Sources/.
TEST_CASE("model-import: external-sidecar .gltf imports and its sidecars land in Sources")
{
    using namespace editor;

    pipeline::RegisterModelManifestAsset();
    pipeline::RegisterTextureAsset();
    pipeline::RegisterMeshAssets();
    pipeline::RegisterMaterialAsset();
    pipeline::RegisterAnimationAssets();

    const StringView dir = u8"scratch_gltf_import_project";
    auto cleanTree = [&]()
    {
        for (StringView sub : {u8"Content", u8"Cooked", u8"Sources", u8".cache"})
        {
            foundation::vfs::NativeFileSystem fs(PathJoin(dir, sub).AsView(), foundation::core::DefaultAllocator());
            Array<foundation::vfs::DirEntry> tops;
            if (fs.AsEnumerable()->Enumerate(u8"", tops).IsOk())
            {
                for (const auto& top : tops)
                {
                    if (!top.isDirectory)
                    {
                        (void)fs.AsWritable()->Delete(top.name.AsView());
                        continue;
                    }
                    Array<foundation::vfs::DirEntry> inner;
                    if (fs.AsEnumerable()->Enumerate(top.name.AsView(), inner).IsOk())
                    {
                        for (const auto& e : inner)
                        {
                            (void)fs.AsWritable()->Delete(
                                PathJoin(top.name.AsView(), e.name.AsView()).AsView());
                        }
                    }
                    (void)RemoveDirectory(
                        PathJoin(PathJoin(dir, sub).AsView(), top.name.AsView()).AsView());
                }
            }
            (void)RemoveDirectory(PathJoin(dir, sub).AsView());
        }
        FileDelete(PathJoin(dir, u8"Project.xml"));
        (void)RemoveDirectory(PathJoin(dir, u8"Editor"));
        (void)RemoveDirectory(dir);
    };
    cleanTree();
    REQUIRE(EditorProject::Create(DefaultAllocator(), dir, u8"P").IsOk());
    UniquePtr<EditorProject> project = EditorProject::Open(DefaultAllocator(), dir);
    REQUIRE(static_cast<bool>(project));

    pipeline::ModelFileImporter importer;
    Result<foundation::content::Instance*> imported =
        importer.Import(MiFox().AsView(),
                        pipeline::ImportContext{DefaultAllocator(), project->SourcesRoot()}, *project->SourceDb().RootGroup(), nullptr, nullptr, nullptr);
    REQUIRE(imported.HasValue());
    REQUIRE(imported.Value() != nullptr);

    // The main file and both referenced sidecars are in Sources/.
    CHECK(FileExists(PathJoin(dir, u8"Sources/Fox.gltf").AsView()));
    CHECK(FileExists(PathJoin(dir, u8"Sources/Fox.bin").AsView()));
    CHECK(FileExists(PathJoin(dir, u8"Sources/Texture.png").AsView()));

    // The fan-out produced meshes and the Fox's animation clips.
    RefPtr<ISerializable> object = imported.Value()->ReadObject();
    auto* manifest = Cast<pipeline::ModelManifestAsset>(object.Get());
    REQUIRE(manifest != nullptr);
    CHECK(manifest->manifest.meshGuids.Size() >= 1u);
    CHECK(manifest->manifest.animationGuids.Size() >= 1u); // Survey, Walk, Run

    cleanTree();
}

// A freshly imported model's material, picked on its own, draws textured: the self-contained
// MaterialSource path wires its textures as the composite spawn path does. Full chain: import,
// cook, bind the material, and the albedo texture is installed as the material's default.
TEST_CASE("model-import: a bound material carries its albedo texture")
{
    using namespace editor;

    pipeline::RegisterModelManifestAsset();
    foundation::model::RegisterModelResourceTypes();
    pipeline::RegisterTextureAsset();
    pipeline::RegisterMeshAssets();
    pipeline::RegisterMaterialAsset();
    pipeline::RegisterAnimationAssets();

    const StringView dir = u8"scratch_mat_tex_project";
    auto cleanTree = [&]()
    {
        for (StringView sub : {u8"Content", u8"Cooked", u8"Sources", u8".cache"})
        {
            foundation::vfs::NativeFileSystem fs(PathJoin(dir, sub).AsView(), foundation::core::DefaultAllocator());
            Array<foundation::vfs::DirEntry> tops;
            if (fs.AsEnumerable()->Enumerate(u8"", tops).IsOk())
            {
                for (const auto& top : tops)
                {
                    if (!top.isDirectory)
                    {
                        (void)fs.AsWritable()->Delete(top.name.AsView());
                        continue;
                    }
                    Array<foundation::vfs::DirEntry> inner;
                    if (fs.AsEnumerable()->Enumerate(top.name.AsView(), inner).IsOk())
                    {
                        for (const auto& e : inner)
                        {
                            (void)fs.AsWritable()->Delete(
                                PathJoin(top.name.AsView(), e.name.AsView()).AsView());
                        }
                    }
                    (void)RemoveDirectory(
                        PathJoin(PathJoin(dir, sub).AsView(), top.name.AsView()).AsView());
                }
            }
            (void)RemoveDirectory(PathJoin(dir, sub).AsView());
        }
        FileDelete(PathJoin(dir, u8"Project.xml"));
        (void)RemoveDirectory(PathJoin(dir, u8"Editor"));
        (void)RemoveDirectory(dir);
    };
    cleanTree();
    REQUIRE(EditorProject::Create(DefaultAllocator(), dir, u8"P").IsOk());
    UniquePtr<EditorProject> project = EditorProject::Open(DefaultAllocator(), dir);
    REQUIRE(static_cast<bool>(project));

    // Import the Duck (textured, static) and cook everything.
    pipeline::ModelFileImporter importer;
    Result<foundation::content::Instance*> imported =
        importer.Import(MiDuck().AsView(),
                        pipeline::ImportContext{DefaultAllocator(), project->SourcesRoot()}, *project->SourceDb().RootGroup(), nullptr, nullptr, nullptr);
    REQUIRE(imported.HasValue());

    RefPtr<ISerializable> object = imported.Value()->ReadObject();
    auto* manifest = Cast<pipeline::ModelManifestAsset>(object.Get());
    REQUIRE(manifest != nullptr);
    REQUIRE(manifest->manifest.materialGuids.Size() >= 1u);
    const Guid matGuid = manifest->manifest.materialGuids[0];

    BuilderRegistry builders{DefaultAllocator()};
    auto add = [&](auto* builder)
    { builders.Register(UniquePtr<IAssetBuilder>(builder, DefaultAllocator())); };
    add(DefaultAllocator().New<pipeline::TextureAssetBuilder>());
    add(DefaultAllocator().New<pipeline::StaticMeshAssetBuilder>());
    add(DefaultAllocator().New<pipeline::SkinnedMeshAssetBuilder>());
    add(DefaultAllocator().New<pipeline::MaterialAssetBuilder>());
    add(DefaultAllocator().New<pipeline::SkeletonAssetBuilder>());
    add(DefaultAllocator().New<pipeline::AnimationClipAssetBuilder>());
    add(DefaultAllocator().New<pipeline::ModelManifestAssetBuilder>());

    foundation::vfs::NativeFileSystem sourcesFs(project->SourcesRoot().AsView(), foundation::core::DefaultAllocator());
    foundation::vfs::NativeFileSystem cacheFs(project->CacheRoot().AsView(), foundation::core::DefaultAllocator());
    CookDriver driver(DefaultAllocator(), project->SourceDb(), project->CookedDb(), builders, &sourcesFs, &cacheFs);
    CookPlan plan = driver.Plan();
    CookStats stats = driver.Execute(plan);
    REQUIRE(stats.failed == 0u);

    // Bind the material like the editor does (null GPU device backs the texture factory).
    // The device comes first: it must outlive the manager's cached products (their destructors release
    // GPU objects through it).
    foundation::rhi::null::NullDevice device{DefaultAllocator()};
    resource::ResourceManager resources(DefaultAllocator(), project->CookedDb());
    foundation::geometry::StaticMeshFactory meshFactory(DefaultAllocator());
    foundation::materials::MaterialFactory materialFactory;
    foundation::texture::TextureFactory textureFactory(DefaultAllocator(), device);
    resources.AddFactory(&meshFactory);
    resources.AddFactory(&materialFactory);
    resources.AddFactory(&textureFactory);

    resource::Proxy<foundation::materials::Material> material =
        resources.Bind<foundation::materials::Material>(matGuid);
    REQUIRE(static_cast<bool>(material));

    // The albedo default texture must be installed on the AlbedoMap property.
    i32 albedoIndex = -1;
    const auto props = material->Properties();
    for (usize i = 0; i < props.Size(); ++i)
    {
        if (props[i].name == StringView(u8"AlbedoMap"))
        {
            albedoIndex = static_cast<i32>(i);
        }
    }
    REQUIRE(albedoIndex >= 0);
    CHECK(material->GetDefaultTexture(static_cast<usize>(albedoIndex)) != nullptr);

    cleanTree();
}

TEST_CASE("importer: FBX separate metal/rough maps bake into one packed MR texture")
{
    // Two 2x2 grayscale sources: roughness = 200, metalness = 60. The packed result must be
    // glTF-convention (G = roughness, B = metalness; R = A = 255); feeding either source
    // directly into the packed slot would bleed its value across both channels.
    model::Model mdl;
    const auto makeGray = [&](u8 value) -> i32
    {
        auto* tex = new model::ModelTexture();
        u8 px[2 * 2 * 4];
        for (u32 i = 0; i < 4; ++i)
        {
            px[i * 4] = value;
            px[i * 4 + 1] = value;
            px[i * 4 + 2] = value;
            px[i * 4 + 3] = 255;
        }
        tex->width = 2;
        tex->height = 2;
        tex->setData(static_cast<const u8*>(px), static_cast<usize>(sizeof(px)));
        return mdl.addTexture(tex);
    };
    const i32 roughIdx = makeGray(200);
    const i32 metalIdx = makeGray(60);

    u32 w = 0, h = 0;
    Array<u8> packed =
        pipeline::BakePackedMetallicRoughness(mdl, roughIdx, metalIdx, w, h);
    REQUIRE(packed.Size() == 2u * 2u * 4u);
    CHECK(w == 2u);
    CHECK(h == 2u);
    CHECK(packed[0] == 255u); // R unused
    CHECK(packed[1] == 200u); // G = roughness
    CHECK(packed[2] == 60u);  // B = metalness
    CHECK(packed[3] == 255u);

    // A missing map bakes identity (255) so the scalar factor carries the value.
    Array<u8> roughOnly =
        pipeline::BakePackedMetallicRoughness(mdl, roughIdx, -1, w, h);
    REQUIRE(roughOnly.Size() == 2u * 2u * 4u);
    CHECK(roughOnly[1] == 200u);
    CHECK(roughOnly[2] == 255u);

    // Usage classification: both sources are data maps, so linear.
    Array<bool> linear;
    auto* mat = new model::ModelMaterial();
    mat->separateRoughnessTextureIndex = roughIdx;
    mat->separateMetalnessTextureIndex = metalIdx;
    mdl.addMaterial(mat);
    pipeline::ClassifyLinearTextures(mdl, linear);
    REQUIRE(linear.Size() == 2u);
    CHECK(linear[0]);
    CHECK(linear[1]);
}

TEST_CASE("mesh convert: missing tangent stream generates tangents (DamagedHelmet class)")
{
    // A quad in the XY plane, normal +Z, with U mapped along +Y, so the generated tangent
    // must be ~(0,1,0), not the {1,0,0} default a missing stream would otherwise leave behind.
    struct SrcVertex
    {
        Float3 pos;
        Float3 normal;
        Float2 uv;
    };
    const SrcVertex verts[4] = {
        {{0, 0, 0}, {0, 0, 1}, {0, 0}},
        {{1, 0, 0}, {0, 0, 1}, {0, 1}},
        {{1, 1, 0}, {0, 0, 1}, {1, 1}},
        {{0, 1, 0}, {0, 0, 1}, {1, 0}},
    };
    const u32 indices[6] = {0, 1, 2, 0, 2, 3};

    model::ModelMesh mesh;
    mesh.addVertexElement(model::VertexElement(model::VertexSemantic::Position,
                                               model::VertexElementFormat::Float3, 0));
    mesh.addVertexElement(model::VertexElement(model::VertexSemantic::Normal,
                                               model::VertexElementFormat::Float3, 12));
    mesh.addVertexElement(model::VertexElement(model::VertexSemantic::TexCoord,
                                               model::VertexElementFormat::Float2, 24));
    mesh.allocateVertices(4, sizeof(SrcVertex));
    mesh.setVertexData(verts, 4);
    mesh.allocateIndices(6, true);
    mesh.setIndexData(indices, 6);

    geometry::StaticMeshSource source;
    pipeline::StaticMeshSourceFromModel(mesh, source);
    REQUIRE(source.vertexBlob.Size() == 4 * sizeof(geometry::StaticMeshVertex));
    const auto* out = reinterpret_cast<const geometry::StaticMeshVertex*>(source.vertexBlob.Data());
    for (usize i = 0; i < 4; ++i)
    {
        const Float4 t = out[i].tangent;
        CHECK(Abs(t.y) == doctest::Approx(1.0f).epsilon(0.01)); // U runs along +Y
        CHECK(Abs(t.x) < 0.01f);
        CHECK(Abs(t.z) < 0.01f);
        // This U-along-Y mapping is mirrored: B = dP/dv = +X but cross(N,T) = -X, so the
        // generated handedness must be -1 (which also shows w isn't stuck at the +1 default).
        CHECK(t.w == doctest::Approx(-1.0f));
        CHECK(Abs(Dot(Float3{t.x, t.y, t.z}, out[i].normal)) < 0.01f);
    }

    // An authored tangent stream passes through untouched (no regeneration).
    struct SrcVertexT
    {
        Float3 pos;
        Float3 normal;
        Float2 uv;
        Float4 tangent;
    };
    const Float4 authored{0, 0, 1, -1};
    SrcVertexT tverts[4];
    for (usize i = 0; i < 4; ++i)
    {
        tverts[i] = {verts[i].pos, verts[i].normal, verts[i].uv, authored};
    }
    model::ModelMesh tmesh;
    tmesh.addVertexElement(model::VertexElement(model::VertexSemantic::Position,
                                                model::VertexElementFormat::Float3, 0));
    tmesh.addVertexElement(model::VertexElement(model::VertexSemantic::Normal,
                                                model::VertexElementFormat::Float3, 12));
    tmesh.addVertexElement(model::VertexElement(model::VertexSemantic::TexCoord,
                                                model::VertexElementFormat::Float2, 24));
    tmesh.addVertexElement(model::VertexElement(model::VertexSemantic::Tangent,
                                                model::VertexElementFormat::Float4, 32));
    tmesh.allocateVertices(4, sizeof(SrcVertexT));
    tmesh.setVertexData(tverts, 4);
    tmesh.allocateIndices(6, true);
    tmesh.setIndexData(indices, 6);

    geometry::StaticMeshSource tsource;
    pipeline::StaticMeshSourceFromModel(tmesh, tsource);
    const auto* tout =
        reinterpret_cast<const geometry::StaticMeshVertex*>(tsource.vertexBlob.Data());
    for (usize i = 0; i < 4; ++i)
    {
        CHECK(tout[i].tangent.z == doctest::Approx(1.0f));
        CHECK(tout[i].tangent.w == doctest::Approx(-1.0f));
    }
}

// Delete a model's group, reimport the same file, and recook: every product keeps the invariant
// that its guid is its source's. The risk is EnsureProduct adopting a same-named cooked instance
// from the previous generation through CreateInstanceWithId's return-existing-by-name behaviour,
// which leaves a misparsed texture product (garbage image parameters). This walks that flow at
// the DB level.
TEST_CASE("cook: delete group -> reimport -> recook keeps product identities clean")
{
    using namespace editor;

    pipeline::RegisterModelManifestAsset();
    pipeline::RegisterTextureAsset();
    pipeline::RegisterMeshAssets();
    pipeline::RegisterMaterialAsset();
    pipeline::RegisterAnimationAssets();
    // Product type registration (the test reads a texture product back).
    GlobalTypeRegistry().Register(foundation::texture::TextureResource::StaticType());
    RegisterSerializable<foundation::texture::TextureResource>();

    const StringView dir = u8"scratch_reimport_identity_project";
    auto cleanTree = [&]()
    {
        for (StringView sub : {u8"Content", u8"Cooked", u8"Sources", u8".cache"})
        {
            foundation::vfs::NativeFileSystem fs(PathJoin(dir, sub).AsView(), foundation::core::DefaultAllocator());
            Array<foundation::vfs::DirEntry> tops;
            if (fs.AsEnumerable()->Enumerate(u8"", tops).IsOk())
            {
                for (const auto& top : tops)
                {
                    if (!top.isDirectory)
                    {
                        (void)fs.AsWritable()->Delete(top.name.AsView());
                        continue;
                    }
                    Array<foundation::vfs::DirEntry> inner;
                    if (fs.AsEnumerable()->Enumerate(top.name.AsView(), inner).IsOk())
                    {
                        for (const auto& e : inner)
                        {
                            (void)fs.AsWritable()->Delete(
                                PathJoin(top.name.AsView(), e.name.AsView()).AsView());
                        }
                    }
                    (void)RemoveDirectory(
                        PathJoin(PathJoin(dir, sub).AsView(), top.name.AsView()).AsView());
                }
            }
            (void)RemoveDirectory(PathJoin(dir, sub).AsView());
        }
        FileDelete(PathJoin(dir, u8"Project.xml"));
        (void)RemoveDirectory(PathJoin(dir, u8"Editor"));
        (void)RemoveDirectory(dir);
    };
    cleanTree();
    REQUIRE(EditorProject::Create(DefaultAllocator(), dir, u8"P").IsOk());
    UniquePtr<EditorProject> project = EditorProject::Open(DefaultAllocator(), dir);
    REQUIRE(static_cast<bool>(project));

    BuilderRegistry builders{DefaultAllocator()};
    auto add = [&](auto* builder)
    { builders.Register(UniquePtr<IAssetBuilder>(builder, DefaultAllocator())); };
    add(DefaultAllocator().New<pipeline::TextureAssetBuilder>());
    add(DefaultAllocator().New<pipeline::StaticMeshAssetBuilder>());
    add(DefaultAllocator().New<pipeline::SkinnedMeshAssetBuilder>());
    add(DefaultAllocator().New<pipeline::MaterialAssetBuilder>());
    add(DefaultAllocator().New<pipeline::SkeletonAssetBuilder>());
    add(DefaultAllocator().New<pipeline::AnimationClipAssetBuilder>());
    add(DefaultAllocator().New<pipeline::ModelManifestAssetBuilder>());

    foundation::vfs::NativeFileSystem sourcesFs(project->SourcesRoot().AsView(), foundation::core::DefaultAllocator());
    foundation::vfs::NativeFileSystem cacheFs(project->CacheRoot().AsView(), foundation::core::DefaultAllocator());
    CookDriver driver(DefaultAllocator(), project->SourceDb(), project->CookedDb(), builders, &sourcesFs, &cacheFs);

    // Duck has a texture (the product kind a misparse breaks). Import and cook generation 1.
    pipeline::ModelFileImporter importer;
    Result<foundation::content::Instance*> firstImport =
        importer.Import(MiDuck().AsView(),
                        pipeline::ImportContext{DefaultAllocator(), project->SourcesRoot()}, *project->SourceDb().RootGroup(), nullptr, nullptr, nullptr);
    REQUIRE(firstImport.HasValue());

    foundation::content::Group* duckGroup = project->SourceDb().RootGroup()->GetGroup(u8"Duck");
    REQUIRE(duckGroup != nullptr);
    Array<Guid> oldGuids;
    for (foundation::content::Instance* inst : duckGroup->Instances())
    {
        oldGuids.PushBack(inst->Id());
    }
    const usize assetCount = oldGuids.Size();
    REQUIRE(assetCount >= 3u); // manifest, mesh, material, texture

    CookPlan plan1 = driver.Plan();
    CookStats stats1 = driver.Execute(plan1);
    CHECK(stats1.failed == 0u);
    for (const Guid& id : oldGuids)
    {
        CHECK(project->CookedDb().GetInstance(id) != nullptr);
    }

    // === step 1: delete the group (source side only, as DeleteGroupNow does) ===
    REQUIRE(project->SourceDb().DeleteGroup(*duckGroup).IsOk());

    // === step 2: reimport the same file ===
    Result<foundation::content::Instance*> secondImport =
        importer.Import(MiDuck().AsView(),
                        pipeline::ImportContext{DefaultAllocator(), project->SourcesRoot()}, *project->SourceDb().RootGroup(), nullptr, nullptr, nullptr);
    REQUIRE(secondImport.HasValue());
    foundation::content::Group* duckGroup2 = project->SourceDb().RootGroup()->GetGroup(u8"Duck");
    REQUIRE(duckGroup2 != nullptr);
    Array<Guid> newGuids;
    for (foundation::content::Instance* inst : duckGroup2->Instances())
    {
        newGuids.PushBack(inst->Id());
    }
    REQUIRE(newGuids.Size() == assetCount);

    // === step 3: the next cook (sweeps generation-1 orphans, cooks generation 2) ===
    CookPlan plan2 = driver.Plan();
    CHECK(plan2.orphans.Size() == assetCount); // every dead source must be an orphan
    CookStats stats2 = driver.Execute(plan2);
    CHECK(stats2.failed == 0u);

    // Identity invariant: every generation-2 product exists under its source guid and
    // matches its source's name; no generation-1 product survives.
    for (usize i = 0; i < newGuids.Size(); ++i)
    {
        const Guid& id = newGuids[i];
        const bool isNew = [&]
        {
            for (const Guid& g : oldGuids)
            {
                if (g == id)
                    return false;
            }
            return true;
        }();
        CHECK(isNew); // reimport minted fresh guids (otherwise this test tests nothing)
        foundation::content::Instance* product = project->CookedDb().GetInstance(id);
        REQUIRE(product != nullptr);
        foundation::content::Instance* source = project->SourceDb().GetInstance(id);
        REQUIRE(source != nullptr);
        CHECK(product->Name() == source->Name());
    }
    for (const Guid& id : oldGuids)
    {
        CHECK(project->CookedDb().GetInstance(id) == nullptr);
    }

    // Identity guard: plant a stale same-named product with a foreign guid and a cross-typed
    // product under a real source guid, then force a recook. EnsureProduct must remove both and
    // land every product on its source's guid and type.
    {
        foundation::content::Group* cookedDuck = project->CookedDb().RootGroup()->GetGroup(u8"Duck");
        REQUIRE(cookedDuck != nullptr);
        // (a) name collision: same name as a real product, different guid.
        foundation::content::Instance* real = nullptr;
        for (const Guid& id : newGuids)
        {
            if (foundation::content::Instance* p = project->CookedDb().GetInstance(id))
            {
                real = p;
                break;
            }
        }
        REQUIRE(real != nullptr);
        const String collidedName(real->Name());
        const Guid foreign = Guid{0xDEADBEEFull, 0xFEEDF00Dull};
        (void)project->CookedDb().DeleteInstance(real->Id());
        foundation::content::Instance* stale = cookedDuck->CreateInstanceWithId(
            foreign, collidedName.AsView(), foundation::texture::TextureResource::StaticType());
        REQUIRE(stale != nullptr);
        CHECK(stale->Id() == foreign);

        CookPlan replan = driver.Plan(true); // force: every asset re-cooks
        CookStats restats = driver.Execute(replan);
        CHECK(restats.failed == 0u);
        for (const Guid& id : newGuids)
        {
            foundation::content::Instance* product = project->CookedDb().GetInstance(id);
            REQUIRE(product != nullptr);
            foundation::content::Instance* source = project->SourceDb().GetInstance(id);
            REQUIRE(source != nullptr);
            CHECK(product->Name() == source->Name());
        }
        CHECK(project->CookedDb().GetInstance(foreign) == nullptr); // stale removed
    }

    // Scoped plans: PlanFor(roots) covers only the roots and their dependency closure.
    {
        Guid matGuid, meshGuid;
        for (foundation::content::Instance* inst : duckGroup2->Instances())
        {
            if (inst->TypeName() == StringView(u8"MaterialAsset"))
            {
                matGuid = inst->Id();
            }
            else if (inst->TypeName() == StringView(u8"StaticMeshAsset"))
            {
                meshGuid = inst->Id();
            }
        }
        REQUIRE(!matGuid.IsNil());
        REQUIRE(!meshGuid.IsNil());

        // Everything is clean after the full cook: a scoped un-forced plan is empty.
        Guid meshRoots[] = {meshGuid};
        CookPlan clean = driver.PlanFor(Span<const Guid>{meshRoots, 1});
        CHECK(clean.dirty.IsEmpty());
        CHECK(clean.orphans.IsEmpty()); // scoped plans never sweep

        // Force re-cooks the root only; its clean dependency closure (textures) stays out.
        Guid matRoots[] = {matGuid};
        CookPlan forced = driver.PlanFor(Span<const Guid>{matRoots, 1}, true);
        REQUIRE(forced.dirty.Size() == 1u);
        CHECK(forced.dirty[0].source == matGuid);
        CookStats scopedStats = driver.Execute(forced);
        CHECK(scopedStats.failed == 0u);
        CHECK(scopedStats.cooked == 1u);
    }

    // The texture product must deserialize to a sane TextureResource (a misparsed product
    // shows garbage width, height and mips).
    bool textureChecked = false;
    for (const Guid& id : newGuids)
    {
        foundation::content::Instance* product = project->CookedDb().GetInstance(id);
        if (product == nullptr || product->TypeName() != StringView(u8"TextureResource"))
        {
            continue;
        }
        RefPtr<ISerializable> object = project->CookedDb().ReadObject(id);
        auto* tex = Cast<foundation::texture::TextureResource>(object.Get());
        REQUIRE(tex != nullptr);
        CHECK(tex->width > 0u);
        CHECK(tex->width < 65536u);
        CHECK(tex->height > 0u);
        CHECK(tex->height < 65536u);
        CHECK(tex->mipLevels > 0u);
        textureChecked = true;
    }
    CHECK(textureChecked);

    cleanTree();
}

TEST_CASE("import: texture assets keep their source names")
{
    model::ModelTexture named;
    named.setName(u8"BaseColor");
    CHECK(pipeline::ImportedTextureName(named, 0).AsView() == StringView(u8"BaseColor"));

    model::ModelTexture fromUri;
    fromUri.setUri(u8"textures/Default_albedo.jpg");
    CHECK(pipeline::ImportedTextureName(fromUri, 3).AsView() ==
          StringView(u8"Default_albedo"));

    model::ModelTexture bare; // embedded, no identity: indexed fallback
    CHECK(pipeline::ImportedTextureName(bare, 7).AsView() == StringView(u8"tex.7"));
}

TEST_CASE("import: material sampler modes map from the source texture's sampler")
{
    CHECK(pipeline::AddressModeFromWrap(model::TextureWrap::Repeat) == 0);
    CHECK(pipeline::AddressModeFromWrap(model::TextureWrap::MirroredRepeat) == 1);
    CHECK(pipeline::AddressModeFromWrap(model::TextureWrap::ClampToEdge) == 2);
}

TEST_CASE("import: sub-assets keep authored names (sanitized), indexed fallback otherwise")
{
    CHECK(pipeline::ImportedAssetName(u8"Material_MR", u8"mat", 0).AsView() ==
          StringView(u8"Material_MR"));
    CHECK(pipeline::ImportedAssetName(u8"mesh_helmet_LP", u8"mesh", 3).AsView() ==
          StringView(u8"mesh_helmet_LP"));
    // Path-hostile characters sanitize (names become envelope file names).
    CHECK(pipeline::ImportedAssetName(u8"body/armor:v2", u8"mesh", 0).AsView() ==
          StringView(u8"body_armor_v2"));
    // No authored name: indexed fallback.
    CHECK(pipeline::ImportedAssetName(u8"", u8"anim", 4).AsView() == StringView(u8"anim.4"));
}

namespace
{
    // Recursive best-effort cleanup so EditorProject::Create starts from a clean slate
    // across test runs (same shape as the first test's cleanTree lambda, two levels deep).
    void CleanProjectTree(StringView dir)
    {
        for (StringView sub : {u8"Content", u8"Cooked", u8"Sources", u8".cache"})
        {
            foundation::vfs::NativeFileSystem fs(foundation::core::PathJoin(dir, sub).AsView(), foundation::core::DefaultAllocator());
            foundation::core::Array<foundation::vfs::DirEntry> tops;
            if (!fs.AsEnumerable()->Enumerate(u8"", tops).IsOk())
            {
                continue;
            }
            for (const auto& top : tops)
            {
                if (!top.isDirectory)
                {
                    (void)fs.AsWritable()->Delete(top.name.AsView());
                    continue;
                }
                foundation::core::Array<foundation::vfs::DirEntry> inner;
                if (fs.AsEnumerable()->Enumerate(top.name.AsView(), inner).IsOk())
                {
                    for (const auto& e : inner)
                    {
                        foundation::core::String path =
                            foundation::core::PathJoin(top.name.AsView(), e.name.AsView());
                        (void)fs.AsWritable()->Delete(path.AsView());
                    }
                }
                (void)fs.AsWritable()->Delete(top.name.AsView());
            }
            (void)foundation::core::RemoveDirectory(foundation::core::PathJoin(dir, sub).AsView());
        }
        foundation::vfs::NativeFileSystem fs(dir, foundation::core::DefaultAllocator());
        foundation::core::Array<foundation::vfs::DirEntry> entries;
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
        (void)foundation::core::RemoveDirectory(dir);
    }
}

TEST_CASE("model-import: options gate textures/materials/animations")
{
    using namespace editor;
    pipeline::RegisterModelManifestAsset();
    pipeline::RegisterTextureAsset();
    pipeline::RegisterMeshAssets();
    pipeline::RegisterMaterialAsset();
    pipeline::RegisterAnimationAssets();

    const StringView dir = u8"scratch_model_import_options_project";
    CleanProjectTree(dir);
    REQUIRE(EditorProject::Create(DefaultAllocator(), dir, u8"P").IsOk());
    UniquePtr<EditorProject> project = EditorProject::Open(DefaultAllocator(), dir);
    REQUIRE(static_cast<bool>(project));

    pipeline::ModelFileImporter importer;

    // The importer advertises options, and their defaults import everything.
    RefPtr<ImportOptions> base = importer.CreateOptions(DefaultAllocator());
    REQUIRE(base.Get() != nullptr);
    auto* options = static_cast<pipeline::ModelImportOptions*>(base.Get());
    CHECK(options->importTextures);
    CHECK(options->importMaterials);
    CHECK(options->importAnimations);
    CHECK(options->generatePrefab);
    CHECK_FALSE(options->generateScene); // opt-in
    CHECK_FALSE(options->generateCollision); // opt-in
    CHECK_FALSE(options->collisionConvex);
    CHECK(options->Toggles().Size() == 9u); // including Root motion and Generate LODs

    // Geometry-only import: no textures, no materials, no skeleton or clips in the fan-out.
    options->importTextures = false;
    options->importMaterials = false;
    options->importAnimations = false;
    Result<foundation::content::Instance*> imported =
        importer.Import(MiGlb().AsView(),
                        pipeline::ImportContext{DefaultAllocator(), project->SourcesRoot()}, *project->SourceDb().RootGroup(), options, nullptr, nullptr);
    REQUIRE(imported.HasValue());

    RefPtr<ISerializable> object = imported.Value()->ReadObject();
    auto* manifest = Cast<pipeline::ModelManifestAsset>(object.Get());
    REQUIRE(manifest != nullptr);
    CHECK(manifest->manifest.meshGuids.Size() >= 1u); // geometry always imports
    CHECK(manifest->manifest.materialGuids.IsEmpty());
    CHECK(manifest->manifest.skeletonGuid.IsNil());
    CHECK(manifest->manifest.animationGuids.IsEmpty());

    foundation::content::Group* modelGroup =
        project->SourceDb().RootGroup()->GetGroup(u8"character-oozi");
    REQUIRE(modelGroup != nullptr);
    for (foundation::content::Instance* inst : modelGroup->Instances())
    {
        CHECK(inst->TypeName() != StringView(u8"TextureAsset"));
        CHECK(inst->TypeName() != StringView(u8"MaterialAsset"));
        CHECK(inst->TypeName() != StringView(u8"SkeletonAsset"));
        CHECK(inst->TypeName() != StringView(u8"AnimationClipAsset"));
    }
}

TEST_CASE("model-import: generate-collision emits CollisionShapeAssets wired to the meshes")
{
    using namespace editor;
    pipeline::RegisterModelManifestAsset();
    pipeline::RegisterTextureAsset();
    pipeline::RegisterMeshAssets();
    pipeline::RegisterMaterialAsset();
    pipeline::RegisterAnimationAssets();
    pipeline::RegisterPhysicsAssets();

    const StringView dir = u8"scratch_model_import_collision_project";
    CleanProjectTree(dir);
    REQUIRE(EditorProject::Create(DefaultAllocator(), dir, u8"P").IsOk());
    UniquePtr<EditorProject> project = EditorProject::Open(DefaultAllocator(), dir);
    REQUIRE(static_cast<bool>(project));

    pipeline::ModelFileImporter importer;
    RefPtr<ImportOptions> base = importer.CreateOptions(DefaultAllocator());
    auto* options = static_cast<pipeline::ModelImportOptions*>(base.Get());
    options->generateCollision = true;
    options->collisionConvex = true;
    Result<foundation::content::Instance*> imported =
        importer.Import(MiGlb().AsView(),
                        pipeline::ImportContext{DefaultAllocator(), project->SourcesRoot()}, *project->SourceDb().RootGroup(), options, nullptr, nullptr);
    REQUIRE(imported.HasValue());

    RefPtr<ISerializable> object = imported.Value()->ReadObject();
    auto* manifest = Cast<pipeline::ModelManifestAsset>(object.Get());
    REQUIRE(manifest != nullptr);
    // collisionGuids parallels meshGuids; static meshes get shapes (skinned stay nil).
    REQUIRE(manifest->manifest.collisionGuids.Size() == manifest->manifest.meshGuids.Size());
    usize shapeCount = 0;
    for (usize i = 0; i < manifest->manifest.collisionGuids.Size(); ++i)
    {
        const Guid& g = manifest->manifest.collisionGuids[i];
        if (manifest->manifest.meshSkinned[i] != 0)
        {
            CHECK(g.IsNil());
            continue;
        }
        REQUIRE(!g.IsNil());
        ++shapeCount;
        foundation::content::Instance* inst = project->SourceDb().GetInstance(g);
        REQUIRE(inst != nullptr);
        RefPtr<ISerializable> shapeObject = inst->ReadObject();
        auto* shape = Cast<pipeline::CollisionShapeAsset>(shapeObject.Get());
        REQUIRE(shape != nullptr);
        CHECK(shape->sourceMesh == manifest->manifest.meshGuids[i]);
        CHECK(shape->cook == pipeline::CollisionCookKind::ConvexHull);
    }
    const bool anySkinned = [&]
    {
        for (u8 skinned : manifest->manifest.meshSkinned)
        {
            if (skinned != 0)
            {
                return true;
            }
        }
        return false;
    }();
    CHECK((shapeCount > 0 || anySkinned));
}

TEST_CASE("model-import: re-import WITHOUT delete reuses instances (same guids, no duplicates)")
{
    using namespace editor;
    pipeline::RegisterModelManifestAsset();
    pipeline::RegisterTextureAsset();
    pipeline::RegisterMeshAssets();
    pipeline::RegisterMaterialAsset();
    pipeline::RegisterAnimationAssets();

    const StringView dir = u8"scratch_model_reimport_project";
    CleanProjectTree(dir);
    REQUIRE(EditorProject::Create(DefaultAllocator(), dir, u8"P").IsOk());
    UniquePtr<EditorProject> project = EditorProject::Open(DefaultAllocator(), dir);
    REQUIRE(static_cast<bool>(project));

    pipeline::ModelFileImporter importer;
    Result<foundation::content::Instance*> first =
        importer.Import(MiDuck().AsView(),
                        pipeline::ImportContext{DefaultAllocator(), project->SourcesRoot()}, *project->SourceDb().RootGroup(), nullptr, nullptr, nullptr);
    REQUIRE(first.HasValue());

    foundation::content::Group* duck = project->SourceDb().RootGroup()->GetGroup(u8"Duck");
    REQUIRE(duck != nullptr);
    HashMap<String, Guid> before;
    for (foundation::content::Instance* inst : duck->Instances())
    {
        before.InsertOrAssign(String(inst->Name()), inst->Id());
    }
    const usize assetCount = before.Size();
    REQUIRE(assetCount >= 3u);

    // Re-drop the same file with the group intact: every instance is reused by (name, type), so
    // guids survive (placed refs and the prefab keep working) and nothing duplicates as ".2".
    Result<foundation::content::Instance*> second =
        importer.Import(MiDuck().AsView(),
                        pipeline::ImportContext{DefaultAllocator(), project->SourcesRoot()}, *project->SourceDb().RootGroup(), nullptr, nullptr, nullptr);
    REQUIRE(second.HasValue());
    CHECK(second.Value()->Id() == first.Value()->Id());

    foundation::content::Group* duckAfter = project->SourceDb().RootGroup()->GetGroup(u8"Duck");
    REQUIRE(duckAfter != nullptr);
    CHECK(duckAfter->Instances().Size() == assetCount); // no duplicates
    for (foundation::content::Instance* inst : duckAfter->Instances())
    {
        const Guid* old = before.Find(String(inst->Name()));
        REQUIRE(old != nullptr);   // same name set as the first import
        CHECK(*old == inst->Id()); // same guid: reuse, not re-mint
    }
}

TEST_CASE("mesh lod: _LODn suffix parsing (case-insensitive; _LOD0 and non-suffixes stay plain)")
{
    String base;
    CHECK(pipeline::ParseLodSuffix(u8"Foo_LOD1", base) == 1);
    CHECK(base == StringView(u8"Foo"));
    CHECK(pipeline::ParseLodSuffix(u8"Rock_lod2", base) == 2);
    CHECK(base == StringView(u8"Rock"));
    CHECK(pipeline::ParseLodSuffix(u8"Wall_Lod12", base) == 12);
    CHECK(base == StringView(u8"Wall"));
    CHECK(pipeline::ParseLodSuffix(u8"Foo", base) == 0);
    CHECK(pipeline::ParseLodSuffix(u8"Foo_LOD0", base) == 0);  // the base names itself plainly
    CHECK(pipeline::ParseLodSuffix(u8"Foo_LOD", base) == 0);   // no digits
    CHECK(pipeline::ParseLodSuffix(u8"LOD1", base) == 0);      // no underscore or base
    CHECK(pipeline::ParseLodSuffix(u8"Foo_MOD1", base) == 0);  // wrong tag
}

// Submeshes index their mesh's own material slots, so a runtime user of the model needs each
// mesh's slot list: the bound model keeps it, and MeshMaterialIndices gives the model-wide
// material each of a mesh's slots is, the list an entity drawing the mesh binds (without it a
// character's face and eyes draw in the wrong materials).
TEST_CASE("model resource: each mesh keeps its material slots, the materials its submeshes index")
{
    const String characterStorage = MiPath(u8"Samples/Models/QuaterniusCharacter/glTF/Character.gltf");
    const StringView character = characterStorage.AsView();
    if (character.IsEmpty())
    {
        return;
    }
    model::RegisterModelResourceTypes();
    vfs::NativeFileSystem mount(u8"scratch_modelimporter_slots_db", DefaultAllocator());
    content::ContentDatabase db(foundation::core::DefaultAllocator(), mount, foundation::core::BinarySerializerFactory(), u8".rasset");
    Guid modelGuid;
    REQUIRE(pipeline::LoadAndCook(character, db, u8"Character", modelGuid) == model::ModelLoadResult::Ok);

    resource::ResourceManager manager(DefaultAllocator(), db);
    geometry::StaticMeshFactory meshFactory(DefaultAllocator());
    geometry::SkinnedMeshFactory skinnedFactory(DefaultAllocator());
    model::ModelFactory modelFactory;
    foundation::animation::SkeletonFactory skeletonFactory(DefaultAllocator());
    foundation::animation::AnimationClipFactory clipFactory(DefaultAllocator());
    manager.AddFactory(&meshFactory);
    manager.AddFactory(&skinnedFactory);
    manager.AddFactory(&modelFactory);
    manager.AddFactory(&skeletonFactory);
    manager.AddFactory(&clipFactory);
    resource::Proxy<model::ModelResource> model = manager.Bind<model::ModelResource>(modelGuid);
    REQUIRE(model);
    REQUIRE(model->meshMaterialSlots.Size() == model->meshes.Size());

    // Every mesh's submeshes index within its own list, and its list names model-wide materials.
    bool sawSeveral = false;
    Array<i32> indices;
    for (usize m = 0; m < model->meshes.Size(); ++m)
    {
        const geometry::StaticMesh* mesh = model->meshes[m].Get();
        REQUIRE(mesh != nullptr);
        model->MeshMaterialIndices(m, indices);
        REQUIRE_FALSE(indices.IsEmpty());
        sawSeveral = sawSeveral || indices.Size() > 1;
        for (const i32 index : indices)
        {
            CHECK(index >= 0);
            CHECK(static_cast<usize>(index) < model->materials.Size());
        }
        for (const geometry::SubMesh& sub : mesh->subMeshes)
        {
            CHECK(sub.materialIndex < static_cast<i32>(indices.Size()));
        }
    }
    CHECK(sawSeveral); // the character's body draws several of its six materials
}

TEST_CASE("mesh convert: submesh materials become per-mesh slots (first-appearance order), "
          "and the slot list names the model-wide materials")
{
    // Three parts over model-wide materials 7, 2, 7: two slots [7, 2]; submeshes index them.
    struct SrcVertex
    {
        Float3 pos;
    };
    model::ModelMesh mesh;
    mesh.addVertexElement(model::VertexElement(model::VertexSemantic::Position,
                                               model::VertexElementFormat::Float3, 0));
    SrcVertex verts[3] = {{Float3{0, 0, 0}}, {Float3{1, 0, 0}}, {Float3{0, 1, 0}}};
    mesh.allocateVertices(3, sizeof(SrcVertex));
    mesh.setVertexData(verts, 3);
    const u32 indices[9] = {0, 1, 2, 0, 1, 2, 0, 1, 2};
    mesh.allocateIndices(9, true);
    mesh.setIndexData(indices, 9);
    mesh.addPart(model::ModelMeshPart{0, 3, 7});
    mesh.addPart(model::ModelMeshPart{3, 3, 2});
    mesh.addPart(model::ModelMeshPart{6, 3, 7});

    Array<i32> slots;
    pipeline::CollectMeshMaterialSlots(mesh, slots);
    REQUIRE(slots.Size() == 2u);
    CHECK(slots[0] == 7);
    CHECK(slots[1] == 2);

    geometry::StaticMeshSource source;
    pipeline::StaticMeshSourceFromModel(mesh, source);
    REQUIRE(source.subMaterial.Size() == 3u);
    CHECK(source.subMaterial[0] == 0); // slot of material 7
    CHECK(source.subMaterial[1] == 1); // slot of material 2
    CHECK(source.subMaterial[2] == 0); // material 7 again: the same slot

    // A part without a material keeps -1 and claims no slot.
    model::ModelMesh bare;
    bare.addVertexElement(model::VertexElement(model::VertexSemantic::Position,
                                               model::VertexElementFormat::Float3, 0));
    bare.allocateVertices(3, sizeof(SrcVertex));
    bare.setVertexData(verts, 3);
    bare.allocateIndices(3, true);
    bare.setIndexData(indices, 3);
    bare.addPart(model::ModelMeshPart{0, 3, -1});
    Array<i32> none;
    pipeline::CollectMeshMaterialSlots(bare, none);
    CHECK(none.IsEmpty());
    geometry::StaticMeshSource bareSource;
    pipeline::StaticMeshSourceFromModel(bare, bareSource);
    REQUIRE(bareSource.subMaterial.Size() == 1u);
    CHECK(bareSource.subMaterial[0] == -1);
}

TEST_CASE("mesh lod: authored level appends into the base's chain (offset indices, ranges, defaults)")
{
    struct SrcVertex
    {
        Float3 pos;
        Float3 normal;
        Float2 uv;
    };
    const auto makeQuad = [](model::ModelMesh& mesh, f32 xShift, u32 vertexCount, u32 indexCount,
                             const u32* indices)
    {
        Array<SrcVertex> verts;
        for (u32 i = 0; i < vertexCount; ++i)
        {
            verts.PushBack(SrcVertex{Float3{xShift + static_cast<f32>(i), 0, 0}, Float3{0, 0, 1},
                                     Float2{0, 0}});
        }
        mesh.addVertexElement(model::VertexElement(model::VertexSemantic::Position,
                                                   model::VertexElementFormat::Float3, 0));
        mesh.addVertexElement(model::VertexElement(model::VertexSemantic::Normal,
                                                   model::VertexElementFormat::Float3, 12));
        mesh.addVertexElement(model::VertexElement(model::VertexSemantic::TexCoord,
                                                   model::VertexElementFormat::Float2, 24));
        mesh.allocateVertices(static_cast<i32>(vertexCount), sizeof(SrcVertex));
        mesh.setVertexData(verts.Data(), static_cast<i32>(vertexCount));
        mesh.allocateIndices(static_cast<i32>(indexCount), true);
        mesh.setIndexData(indices, static_cast<i32>(indexCount));
    };

    const u32 baseIndices[6] = {0, 1, 2, 0, 2, 3};
    model::ModelMesh baseMesh;
    makeQuad(baseMesh, 0.0f, 4, 6, baseIndices);
    geometry::StaticMeshSource source;
    pipeline::StaticMeshSourceFromModel(baseMesh, source);
    REQUIRE(source.subStart.Size() == 1);

    const u32 lodIndices[3] = {0, 1, 2};
    model::ModelMesh lodMesh;
    makeQuad(lodMesh, 100.0f, 3, 3, lodIndices);
    REQUIRE(pipeline::AppendLodLevelFromModel(lodMesh, source));

    // Chain shape: 2 levels, level 1 one range after the base's 6 indices, indices
    // offset past the base's 4 vertices, default threshold ladder (LOD1 at 0.25).
    CHECK(source.lodCount == 2);
    REQUIRE(source.lodStart.Size() == 1);
    CHECK(source.lodStart[0] == 6);
    CHECK(source.lodIndexCount[0] == 3);
    CHECK(source.vertexBlob.Size() == 7 * sizeof(geometry::StaticMeshVertex));
    CHECK(source.indexData.Size() == 9);
    CHECK(source.indexData[6] == 4); // 0 + 4-vertex offset
    REQUIRE(source.lodCoverage.Size() == 2);
    CHECK(source.lodCoverage[1] == doctest::Approx(0.25f));

    // The level's vertices really are the appended ones (position x carries the shift).
    const auto* verts =
        reinterpret_cast<const geometry::StaticMeshVertex*>(source.vertexBlob.Data());
    CHECK(verts[4].position.x == doctest::Approx(100.0f));

    // A second level extends the ladder.
    model::ModelMesh lod2;
    const u32 lod2Indices[3] = {0, 2, 1};
    makeQuad(lod2, 200.0f, 3, 3, lod2Indices);
    REQUIRE(pipeline::AppendLodLevelFromModel(lod2, source));
    CHECK(source.lodCount == 3);
    CHECK(source.lodCoverage[2] == doctest::Approx(0.125f));

    // Part-count mismatch: refused, base untouched.
    model::ModelMesh twoParts;
    const u32 tpIndices[6] = {0, 1, 2, 0, 2, 1};
    makeQuad(twoParts, 300.0f, 3, 6, tpIndices);
    twoParts.addPart(model::ModelMeshPart{0, 3, 0});
    twoParts.addPart(model::ModelMeshPart{3, 3, 1});
    const usize blobBefore = source.vertexBlob.Size();
    CHECK_FALSE(pipeline::AppendLodLevelFromModel(twoParts, source));
    CHECK(source.vertexBlob.Size() == blobBefore);
    CHECK(source.lodCount == 3);

    // The chain survives the runtime fill (levels slice; LOD 1 draws its own range).
    geometry::StaticMesh mesh;
    source.FillStatic(mesh);
    CHECK(mesh.lodCount == 3);
    CHECK(mesh.SubMeshesForLod(1)[0].startIndex == 6);
    CHECK(mesh.SubMeshesForLod(1)[0].indexCount == 3);
}

TEST_CASE("mesh lod: a skinned level appends with its skinning stream in lockstep")
{
    struct SkinVertex
    {
        Float3 pos;
        Float3 normal;
        Float2 uv;
        u16 joints[4];
        Float4 weights;
    };
    const auto makeSkinnedTri = [](model::ModelMesh& mesh, f32 xShift, u16 joint)
    {
        SkinVertex verts[3];
        for (u32 i = 0; i < 3; ++i)
        {
            verts[i] = {};
            verts[i].pos = Float3{xShift + static_cast<f32>(i), 0, 0};
            verts[i].normal = Float3{0, 0, 1};
            verts[i].joints[0] = joint;
            verts[i].weights = Float4{1, 0, 0, 0};
        }
        const u32 indices[3] = {0, 1, 2};
        mesh.addVertexElement(model::VertexElement(model::VertexSemantic::Position,
                                                   model::VertexElementFormat::Float3, 0));
        mesh.addVertexElement(model::VertexElement(model::VertexSemantic::Normal,
                                                   model::VertexElementFormat::Float3, 12));
        mesh.addVertexElement(model::VertexElement(model::VertexSemantic::TexCoord,
                                                   model::VertexElementFormat::Float2, 24));
        mesh.addVertexElement(model::VertexElement(model::VertexSemantic::Joints,
                                                   model::VertexElementFormat::UShort4, 32));
        mesh.addVertexElement(model::VertexElement(model::VertexSemantic::Weights,
                                                   model::VertexElementFormat::Float4, 40));
        mesh.allocateVertices(3, sizeof(SkinVertex));
        mesh.setVertexData(verts, 3);
        mesh.allocateIndices(3, true);
        mesh.setIndexData(indices, 3);
    };

    model::ModelMesh baseMesh;
    makeSkinnedTri(baseMesh, 0.0f, 7);
    geometry::SkinnedMeshSource source;
    pipeline::SkinnedMeshSourceFromModel(baseMesh, 2, source);
    REQUIRE(source.skinningBlob.Size() == 3 * sizeof(geometry::VertexSkinning));

    model::ModelMesh lodMesh;
    makeSkinnedTri(lodMesh, 50.0f, 9);
    REQUIRE(pipeline::AppendLodLevelFromModel(lodMesh, source));

    // Both streams grew by the level's 3 vertices; the level's entries carry joint 9.
    CHECK(source.lodCount == 2);
    CHECK(source.vertexBlob.Size() == 6 * sizeof(geometry::StaticMeshVertex));
    CHECK(source.skinningBlob.Size() == 6 * sizeof(geometry::VertexSkinning));
    const auto* skin =
        reinterpret_cast<const geometry::VertexSkinning*>(source.skinningBlob.Data());
    CHECK(skin[2].joints[0] == 7); // base entries intact
    CHECK(skin[3].joints[0] == 9); // appended level entries
    CHECK(source.lodStart[0] == 3);
    CHECK(source.indexData[3] == 3); // offset past the base's vertices
}

TEST_CASE("model-import: DescribeImport lists the fan-out; the selection filters and renames it")
{
    using namespace editor;
    using namespace pipeline;
    pipeline::RegisterModelManifestAsset();
    pipeline::RegisterTextureAsset();
    pipeline::RegisterMeshAssets();
    pipeline::RegisterMaterialAsset();
    pipeline::RegisterAnimationAssets();

    const StringView dir = u8"scratch_model_describe_project";
    auto cleanTree = [&]()
    {
        for (StringView sub : {u8"Content", u8"Cooked", u8"Sources", u8".cache"})
        {
            foundation::vfs::NativeFileSystem fs(PathJoin(dir, sub).AsView(), foundation::core::DefaultAllocator());
            Array<foundation::vfs::DirEntry> tops;
            if (fs.AsEnumerable()->Enumerate(u8"", tops).IsOk())
            {
                for (const auto& top : tops)
                {
                    if (!top.isDirectory)
                    {
                        (void)fs.AsWritable()->Delete(top.name.AsView());
                        continue;
                    }
                    Array<foundation::vfs::DirEntry> inner;
                    if (fs.AsEnumerable()->Enumerate(top.name.AsView(), inner).IsOk())
                    {
                        for (const auto& e : inner)
                        {
                            (void)fs.AsWritable()->Delete(
                                PathJoin(top.name.AsView(), e.name.AsView()).AsView());
                        }
                    }
                    (void)RemoveDirectory(
                        PathJoin(PathJoin(dir, sub).AsView(), top.name.AsView()).AsView());
                }
            }
            (void)RemoveDirectory(PathJoin(dir, sub).AsView());
        }
        FileDelete(PathJoin(dir, u8"Project.xml"));
        (void)RemoveDirectory(PathJoin(dir, u8"Editor"));
        (void)RemoveDirectory(dir);
    };
    cleanTree();
    REQUIRE(EditorProject::Create(DefaultAllocator(), dir, u8"P").IsOk());
    UniquePtr<EditorProject> project = EditorProject::Open(DefaultAllocator(), dir);
    REQUIRE(static_cast<bool>(project));

    pipeline::ModelFileImporter importer;
    const String glbStorage = MiGlb();
    const StringView glb = glbStorage.AsView();

    // The plan of a skinned character: meshes, materials, skeleton and clips, all enabled,
    // each target the source name.
    pipeline::ImportPlan plan = importer.DescribeImport(glb, nullptr, nullptr);
    REQUIRE(!plan.IsEmpty());
    usize meshCount = 0, clipCount = 0, skeletonCount = 0;
    for (const pipeline::ImportPlanEntry& e : plan.entries)
    {
        CHECK(e.enabled);
        CHECK(e.targetName.AsView() == e.sourceName.AsView());
        meshCount += (e.kind == pipeline::ImportResourceKind::Mesh) ? 1u : 0u;
        clipCount += (e.kind == pipeline::ImportResourceKind::AnimationClip) ? 1u : 0u;
        skeletonCount += (e.kind == pipeline::ImportResourceKind::Skeleton) ? 1u : 0u;
    }
    REQUIRE(meshCount >= 1u);
    REQUIRE(clipCount >= 1u);
    REQUIRE(skeletonCount == 1u);

    // Parity: an unfiltered import creates an instance for every plan entry, by name.
    {
        Result<foundation::content::Instance*> imported = importer.Import(
            glb, pipeline::ImportContext{DefaultAllocator(), project->SourcesRoot()},
            *project->SourceDb().RootGroup(), nullptr, nullptr, nullptr);
        REQUIRE(imported.HasValue());
        foundation::content::Group& modelGroup = imported.Value()->OwningGroup();
        for (const pipeline::ImportPlanEntry& e : plan.entries)
        {
            CHECK(modelGroup.GetInstance(e.sourceName.AsView()) != nullptr);
        }
    }

    // Selection: clips off, first mesh renamed; the fan-out skips and renames accordingly.
    {
        auto options = MakeRef<pipeline::ModelImportOptions>(DefaultAllocator());
        options->selection = importer.DescribeImport(glb, options.Get(), nullptr);
        String meshSource;
        for (pipeline::ImportPlanEntry& e : options->selection.entries)
        {
            if (e.kind == pipeline::ImportResourceKind::AnimationClip)
            {
                e.enabled = false;
            }
            if (e.kind == pipeline::ImportResourceKind::Mesh && meshSource.IsEmpty())
            {
                meshSource = e.sourceName;
                e.targetName = String(u8"hero.mesh");
            }
        }
        REQUIRE(!meshSource.IsEmpty());

        foundation::content::Group* target =
            project->SourceDb().RootGroup()->CreateGroup(u8"filtered");
        Result<foundation::content::Instance*> imported = importer.Import(
            glb, pipeline::ImportContext{DefaultAllocator(), project->SourcesRoot()}, *target, options.Get(),
            nullptr, nullptr);
        REQUIRE(imported.HasValue());
        foundation::content::Group& modelGroup = imported.Value()->OwningGroup();

        CHECK(modelGroup.GetInstance(u8"hero.mesh") != nullptr);   // renamed
        CHECK(modelGroup.GetInstance(meshSource.AsView()) == nullptr); // not under the old name
        for (foundation::content::Instance* inst : modelGroup.Instances())
        {
            CHECK(inst->TypeName() != StringView(u8"AnimationClipAsset")); // clips skipped
        }
        // The manifest kept the skeleton (still selected) and lost the clips.
        RefPtr<ISerializable> object = imported.Value()->ReadObject();
        auto* manifest = Cast<pipeline::ModelManifestAsset>(object.Get());
        REQUIRE(manifest != nullptr);
        CHECK(!manifest->manifest.skeletonGuid.IsNil());
        CHECK(manifest->manifest.animationGuids.IsEmpty());
        CHECK(!manifest->manifest.meshGuids.IsEmpty());
        CHECK(!manifest->manifest.meshGuids[0].IsNil());

        // Re-import memory: the manifest stored the decisions; StoredSelection reads them
        // back, and merging onto a fresh plan reproduces them (clips stay off, the rename
        // sticks) without the user answering anything again.
        pipeline::ImportPlan stored = importer.StoredSelection(*target, glb);
        REQUIRE(!stored.IsEmpty());
        pipeline::ImportPlan fresh = importer.DescribeImport(glb, nullptr, nullptr);
        pipeline::MergeStoredSelection(fresh, stored);
        bool sawRenamedMesh = false;
        for (const pipeline::ImportPlanEntry& e : fresh.entries)
        {
            if (e.kind == pipeline::ImportResourceKind::AnimationClip)
            {
                CHECK(!e.enabled);
            }
            if (e.sourceName == meshSource)
            {
                sawRenamedMesh = true;
                CHECK(e.targetName == u8"hero.mesh");
            }
        }
        CHECK(sawRenamedMesh);
    }

    cleanTree();
}

TEST_CASE("model-import: LOD folding holds the manifest slot so node mesh indices stay valid")
{
    // Meshes [Part_LOD1, Part]: the level folds into its base but keeps its manifest.meshGuids
    // slot, because node.meshIndex is a model index; without the slot the Part node's index 1
    // would fall out of a 1-entry array and the generated prefab would lose the mesh.
    using namespace editor;
    using namespace pipeline;
    pipeline::RegisterModelManifestAsset();
    pipeline::RegisterMeshAssets();

    const StringView dir = u8"scratch_model_lodslot_project";
    FileDelete(PathJoin(dir, u8"Content/lodtest/Part.xasset"));
    FileDelete(PathJoin(dir, u8"Content/lodtest/Part.geometry.bin"));
    FileDelete(PathJoin(dir, u8"Content/lodtest/lodtest.xasset"));
    (void)RemoveDirectory(PathJoin(dir, u8"Content/lodtest"));
    FileDelete(PathJoin(dir, u8"Sources/lodtest.glb"));
    for (StringView sub : {u8"Content", u8"Cooked", u8"Sources", u8".cache", u8"Editor"})
    {
        (void)RemoveDirectory(PathJoin(dir, sub));
    }
    FileDelete(PathJoin(dir, u8"Project.xml"));
    (void)RemoveDirectory(dir);
    REQUIRE(EditorProject::Create(DefaultAllocator(), dir, u8"P").IsOk());
    UniquePtr<EditorProject> project = EditorProject::Open(DefaultAllocator(), dir);
    REQUIRE(static_cast<bool>(project));

    // The dropped file only needs to exist (provenance copy); the parsed model arrives as
    // the prepared payload, as on the editor's worker-prepare path.
    const String fakeSource = PathJoin(dir, u8"lodtest.glb");
    const byte junk[4] = {byte{1}, byte{2}, byte{3}, byte{4}};
    REQUIRE(WriteFile(fakeSource.AsView(), Span<const byte>(junk, 4)).IsOk());

    struct SrcVertex
    {
        Float3 pos;
        Float3 normal;
        Float2 uv;
    };
    const SrcVertex verts[3] = {
        {{0, 0, 0}, {0, 0, 1}, {0, 0}},
        {{1, 0, 0}, {0, 0, 1}, {0, 1}},
        {{1, 1, 0}, {0, 0, 1}, {1, 1}},
    };
    const u32 indices[3] = {0, 1, 2};
    const auto makeMesh = [&](StringView name) -> model::ModelMesh*
    {
        auto* mesh = new model::ModelMesh();
        mesh->setName(name);
        mesh->addVertexElement(model::VertexElement(model::VertexSemantic::Position,
                                                    model::VertexElementFormat::Float3, 0));
        mesh->addVertexElement(model::VertexElement(model::VertexSemantic::Normal,
                                                    model::VertexElementFormat::Float3, 12));
        mesh->addVertexElement(model::VertexElement(model::VertexSemantic::TexCoord,
                                                    model::VertexElementFormat::Float2, 24));
        mesh->allocateVertices(3, sizeof(SrcVertex));
        mesh->setVertexData(verts, 3);
        mesh->allocateIndices(3, true);
        mesh->setIndexData(indices, 3);
        return mesh;
    };

    auto prepared = MakeRef<pipeline::LoadedModel>(DefaultAllocator());
    // The level sits at index 0 and the base at index 1, the order that would shift the slots.
    (void)prepared->model.addMesh(makeMesh(u8"Part_LOD1"));
    (void)prepared->model.addMesh(makeMesh(u8"Part"));
    auto* node = new model::ModelBone();
    node->setName(u8"PartNode");
    node->meshIndex = 1; // references the base by model index
    (void)prepared->model.addBone(node);
    prepared->model.calculateBounds();

    pipeline::ModelFileImporter importer;
    Result<foundation::content::Instance*> imported = importer.Import(
        fakeSource.AsView(), pipeline::ImportContext{DefaultAllocator(), project->SourcesRoot()},
        *project->SourceDb().RootGroup(), nullptr, prepared.Get(), nullptr);
    REQUIRE(imported.HasValue());

    RefPtr<ISerializable> object = imported.Value()->ReadObject();
    auto* manifestAsset = Cast<pipeline::ModelManifestAsset>(object.Get());
    REQUIRE(manifestAsset != nullptr);
    const foundation::model::ModelManifestSource& manifest = manifestAsset->manifest;

    // Both model meshes hold a slot, the folded level as nil and the base as a real asset,
    // and the node's model-space index resolves to the base's guid.
    REQUIRE(manifest.meshGuids.Size() == 2u);
    CHECK(manifest.meshGuids[0].IsNil());
    CHECK(!manifest.meshGuids[1].IsNil());
    REQUIRE(manifest.nodes.Size() == 1u);
    CHECK(manifest.nodes[0].meshIndex == 1);
    foundation::content::Instance* part =
        imported.Value()->OwningGroup().GetInstance(u8"Part");
    REQUIRE(part != nullptr);
    CHECK(part->Id() == manifest.meshGuids[1]);
    CHECK(imported.Value()->OwningGroup().GetInstance(u8"Part_LOD1") == nullptr);
}

// A .gltf whose sidecars live in a subfolder ("textures/x.jpg"): the deferred provenance copies
// into Sources/textures/ must create their parent directories, as the inline path's
// NativeFileSystem::Save does. A failed copy fails the job, which then skips FinishImport and
// the prefab and scene generators.
TEST_CASE("model-import: nested-subfolder sidecars survive the DEFERRED write path")
{
    using namespace editor;

    pipeline::RegisterModelManifestAsset();
    pipeline::RegisterTextureAsset();
    pipeline::RegisterMeshAssets();
    pipeline::RegisterMaterialAsset();
    pipeline::RegisterAnimationAssets();

    const StringView dir = u8"scratch_gltf_nested_import_project";
    for (StringView sub : {u8"Sources/textures", u8"Sources"})
    {
        foundation::vfs::NativeFileSystem fs(PathJoin(dir, sub).AsView(),
                                             foundation::core::DefaultAllocator());
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
        (void)RemoveDirectory(PathJoin(dir, sub).AsView());
    }
    {
        foundation::vfs::NativeFileSystem fs(PathJoin(dir, u8"Content").AsView(),
                                             foundation::core::DefaultAllocator());
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
        (void)RemoveDirectory(PathJoin(dir, u8"Content").AsView());
    }
    FileDelete(PathJoin(dir, u8"Project.xml"));
    (void)RemoveDirectory(PathJoin(dir, u8"Editor"));
    (void)RemoveDirectory(dir);

    REQUIRE(EditorProject::Create(DefaultAllocator(), dir, u8"P").IsOk());
    UniquePtr<EditorProject> project = EditorProject::Open(DefaultAllocator(), dir);
    REQUIRE(static_cast<bool>(project));

    // Deferred mode is the editor's worker path: PrepareOnWorker loads the model, Import
    // queues the envelope, stream and copy writes, and the caller executes them while the
    // prepared payload stays alive. The deferred views borrow its decoded pixels (the editor's
    // job captures it for this reason; dropping it before Execute is a use-after-free).
    Array<pipeline::DeferredImportWrite> deferred;
    pipeline::ModelFileImporter importer;
    RefPtr<Object> prepared = importer.PrepareOnWorker(
        MiFoxNested().AsView(),
        DefaultAllocator());
    Result<foundation::content::Instance*> imported = importer.Import(
        MiFoxNested().AsView(),
        pipeline::ImportContext{DefaultAllocator(), project->SourcesRoot()},
        *project->SourceDb().RootGroup(), nullptr, prepared.Get(), &deferred);
    REQUIRE(imported.HasValue());
    REQUIRE(imported.Value() != nullptr);
    REQUIRE(!deferred.IsEmpty());

    // Every deferred write must succeed, the nested Sources/textures/ copy included.
    for (pipeline::DeferredImportWrite& write : deferred)
    {
        const Status s = write.Execute();
        CHECK(s.IsOk());
    }
    CHECK(FileExists(PathJoin(dir, u8"Sources/Fox.gltf").AsView()));
    CHECK(FileExists(PathJoin(dir, u8"Sources/Fox.bin").AsView()));
    CHECK(FileExists(PathJoin(dir, u8"Sources/textures/Texture.png").AsView()));

    // Naming prefers the file stem over the authored image name (this fixture's image is
    // named "authored_name_lies", like a packed arm map named "*_rough"), and the asset
    // records its provenance uri for the texture page's Source row.
    foundation::content::Instance* tex =
        imported.Value()->OwningGroup().GetInstance(u8"Texture");
    REQUIRE(tex != nullptr);
    CHECK(imported.Value()->OwningGroup().GetInstance(u8"authored_name_lies") == nullptr);
    RefPtr<ISerializable> texObject = tex->ReadObject();
    auto* texAsset = Cast<pipeline::TextureAsset>(texObject.Get());
    REQUIRE(texAsset != nullptr);
    CHECK(texAsset->sourceHint == u8"textures/Texture.png");
}

// DDS textures referenced by a model (packages such as Bistro ship BC-compressed DDS) are not
// decoded and embedded: the loader leaves them on disk, the importer copies the file into
// Sources/ and creates a file-backed TextureAsset with the slot's usage, and the cook passes
// the GPU-ready levels through (or decodes when they do not fit the policy).
namespace
{
    void PutF32(Array<byte>& out, f32 v)
    {
        byte b[4];
        MemCopy(b, &v, 4);
        for (byte x : b)
        {
            out.PushBack(x);
        }
    }
    void PutU16(Array<byte>& out, u16 v)
    {
        out.PushBack(static_cast<byte>(v & 0xFFu));
        out.PushBack(static_cast<byte>(v >> 8));
    }
    // Solid BC1 (RGB565 endpoints, every index 0) and BC5 (two solid BC4 halves) blocks.
    void Bc1Solid(Array<u8>& out, u16 rgb565)
    {
        out.PushBack(static_cast<u8>(rgb565 & 0xFFu));
        out.PushBack(static_cast<u8>(rgb565 >> 8));
        out.PushBack(static_cast<u8>(rgb565 & 0xFFu));
        out.PushBack(static_cast<u8>(rgb565 >> 8));
        for (u32 i = 0; i < 4; ++i)
        {
            out.PushBack(0);
        }
    }
    void Bc4Solid(Array<u8>& out, u8 v)
    {
        out.PushBack(v);
        out.PushBack(v);
        for (u32 i = 0; i < 6; ++i)
        {
            out.PushBack(0);
        }
    }
    void WriteDdsFile(const foundation::image::dds::DdsImage& image, StringView path)
    {
        Array<u8> file;
        REQUIRE(foundation::image::dds::WriteDds(image, file).IsOk());
        REQUIRE(WriteFile(path, Span<const byte>(reinterpret_cast<const byte*>(file.Data()), file.Size())).IsOk());
    }

    // A one-triangle glTF in `dir` whose material binds albedo.dds (BC1 sRGB, 3 levels) in the
    // base-colour slot and normal.dds (BC5, one level) in the normal slot.
    void WriteDdsTriangle(StringView dir)
    {
        (void)CreateDirectory(dir);
        Array<byte> bin;
        const f32 positions[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
        const f32 normals[9] = {0, 0, 1, 0, 0, 1, 0, 0, 1};
        const f32 uvs[6] = {0, 0, 1, 0, 0, 1};
        for (f32 v : positions) PutF32(bin, v);
        for (f32 v : normals) PutF32(bin, v);
        for (f32 v : uvs) PutF32(bin, v);
        PutU16(bin, 0);
        PutU16(bin, 1);
        PutU16(bin, 2);
        REQUIRE(bin.Size() == 102u);
        REQUIRE(WriteFile(PathJoin(dir, u8"tri.bin").AsView(), Span<const byte>(bin.Data(), bin.Size())).IsOk());
        const StringView json =
            u8"{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
            u8"\"nodes\":[{\"mesh\":0,\"name\":\"Tri\"}],"
            u8"\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"NORMAL\":1,\"TEXCOORD_0\":2},\"indices\":3,\"material\":0}]}],"
            // Bistro's shape: spec-gloss materials, textures whose `source` is a PNG (absent
            // here) with the DDS under MSFT_texture_dds.
            u8"\"extensionsUsed\":[\"KHR_materials_pbrSpecularGlossiness\",\"MSFT_texture_dds\"],"
            u8"\"materials\":[{\"name\":\"Mat\",\"extensions\":{\"KHR_materials_pbrSpecularGlossiness\":{\"diffuseTexture\":{\"index\":0},\"glossinessFactor\":0.25}},\"normalTexture\":{\"index\":1}}],"
            u8"\"textures\":[{\"source\":2,\"extensions\":{\"MSFT_texture_dds\":{\"source\":0}}},{\"source\":3,\"extensions\":{\"MSFT_texture_dds\":{\"source\":1}}}],"
            u8"\"images\":[{\"uri\":\"tex/albedo.dds\"},{\"uri\":\"tex/normal.dds\"},{\"uri\":\"tex/albedo.png\"},{\"uri\":\"tex/normal.png\"}],"
            u8"\"buffers\":[{\"uri\":\"tri.bin\",\"byteLength\":102}],"
            u8"\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},{\"buffer\":0,\"byteOffset\":36,\"byteLength\":36},"
            u8"{\"buffer\":0,\"byteOffset\":72,\"byteLength\":24},{\"buffer\":0,\"byteOffset\":96,\"byteLength\":6}],"
            u8"\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\",\"min\":[0,0,0],\"max\":[1,1,0]},"
            u8"{\"bufferView\":1,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
            u8"{\"bufferView\":2,\"componentType\":5126,\"count\":3,\"type\":\"VEC2\"},"
            u8"{\"bufferView\":3,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"}]}";
        REQUIRE(WriteFile(PathJoin(dir, u8"tri.gltf").AsView(),
                          Span<const byte>(reinterpret_cast<const byte*>(json.Data()), json.Size()))
                    .IsOk());

        foundation::image::dds::DdsImage albedo;
        albedo.width = 4;
        albedo.height = 4;
        albedo.mipLevels = 3;
        albedo.format = foundation::image::dds::DdsFormat::BC1Srgb;
        albedo.colorSpaceKnown = true;
        for (u32 level = 0; level < 3; ++level)
        {
            Bc1Solid(albedo.data, 0xF800);
        }
        (void)CreateDirectory(PathJoin(dir, u8"tex").AsView());
        WriteDdsFile(albedo, PathJoin(dir, u8"tex/albedo.dds").AsView());

        foundation::image::dds::DdsImage normal;
        normal.width = 4;
        normal.height = 4;
        normal.mipLevels = 1;
        normal.format = foundation::image::dds::DdsFormat::BC5;
        normal.colorSpaceKnown = true;
        Bc4Solid(normal.data, 128);
        Bc4Solid(normal.data, 128);
        WriteDdsFile(normal, PathJoin(dir, u8"tex/normal.dds").AsView());
    }
}

TEST_CASE("model-import: DDS textures stay on disk as file-backed assets and pass through the cook")
{
    using namespace editor;
    using namespace pipeline;

    pipeline::RegisterModelManifestAsset();
    pipeline::RegisterTextureAsset();
    pipeline::RegisterMeshAssets();
    pipeline::RegisterMaterialAsset();
    pipeline::RegisterAnimationAssets();
    foundation::texture::RegisterTextureResource();

    const StringView src = u8"scratch_dds_model_src";
    const StringView dir = u8"scratch_dds_model_project";
    (void)RemoveDirectoryRecursive(src);
    (void)RemoveDirectoryRecursive(dir);
    WriteDdsTriangle(src);

    // The loader leaves both DDS files undecoded and remembers where they are.
    {
        model::gltf::GltfLoader gltfLoader;
        model::io::registerLoader(&gltfLoader);
        model::Model loaded;
        REQUIRE(model::io::loadModel(PathJoin(src, u8"tri.gltf").AsView(), loaded) ==
                model::ModelLoadResult::Ok);
        REQUIRE(loaded.textures().Size() == 2u);
        CHECK(loaded.textures()[0]->getData() == nullptr);
        CHECK_FALSE(loaded.textures()[0]->sourceFile().IsEmpty());
        CHECK(loaded.textures()[1]->uri() == StringView(u8"tex/normal.dds")); // the DDS, not the PNG
        REQUIRE(loaded.materials().Size() == 1u);
        CHECK(loaded.materials()[0]->baseColorTextureIndex == 0); // spec-gloss diffuse -> base colour
        CHECK(loaded.materials()[0]->normalTextureIndex == 1);
        CHECK(loaded.materials()[0]->roughnessFactor == doctest::Approx(0.75f));
        CHECK(loaded.materials()[0]->metallicFactor == doctest::Approx(0.0f));
        model::io::unregisterLoader(&gltfLoader); // the registry keeps raw pointers
    }

    REQUIRE(EditorProject::Create(DefaultAllocator(), dir, u8"P").IsOk());
    UniquePtr<EditorProject> project = EditorProject::Open(DefaultAllocator(), dir);
    REQUIRE(static_cast<bool>(project));

    pipeline::ModelFileImporter importer;
    Result<foundation::content::Instance*> imported = importer.Import(
        PathJoin(src, u8"tri.gltf").AsView(),
        pipeline::ImportContext{DefaultAllocator(), project->SourcesRoot()},
        *project->SourceDb().RootGroup(), nullptr, nullptr, nullptr);
    REQUIRE(imported.HasValue());
    REQUIRE(imported.Value() != nullptr);
    // One copy each, at the model-relative path the sidecar pass uses - never a second flat copy.
    CHECK(FileExists(PathJoin(dir, u8"Sources/tex/albedo.dds").AsView()));
    CHECK(FileExists(PathJoin(dir, u8"Sources/tex/normal.dds").AsView()));
    CHECK_FALSE(FileExists(PathJoin(dir, u8"Sources/albedo.dds").AsView()));

    foundation::content::Group* modelGroup = project->SourceDb().RootGroup()->GetGroup(u8"tri");
    REQUIRE(modelGroup != nullptr);
    foundation::content::Instance* albedoInst = modelGroup->GetInstance(u8"albedo");
    foundation::content::Instance* normalInst = modelGroup->GetInstance(u8"normal");
    REQUIRE(albedoInst != nullptr);
    REQUIRE(normalInst != nullptr);
    {
        RefPtr<ISerializable> object = albedoInst->ReadObject();
        auto* asset = Cast<pipeline::TextureAsset>(object.Get());
        REQUIRE(asset != nullptr);
        CHECK(asset->fileName == StringView(u8"tex/albedo.dds")); // file-backed, not embedded
        CHECK(asset->embeddedWidth == 0u);
        CHECK(asset->usage == ::texcomp::TextureUsage::Color);
        CHECK(asset->colorSpace == foundation::image::ImageColorSpace::Srgb); // the DX10 fact
        CHECK(asset->sourceHint == StringView(u8"tex/albedo.dds"));
    }
    {
        RefPtr<ISerializable> object = normalInst->ReadObject();
        auto* asset = Cast<pipeline::TextureAsset>(object.Get());
        REQUIRE(asset != nullptr);
        CHECK(asset->fileName == StringView(u8"tex/normal.dds"));
        CHECK(asset->usage == ::texcomp::TextureUsage::Normal); // the slot
        CHECK(asset->colorSpace == foundation::image::ImageColorSpace::Linear);
    }

    // Cook through the driver: the albedo's BC1 chain passes through untouched; the BC5 normal
    // decodes (the shaders read rgb) and cooks by policy (small: raw RGBA8, generated mips).
    BuilderRegistry builders{DefaultAllocator()};
    auto add = [&](auto* builder)
    { builders.Register(UniquePtr<IAssetBuilder>(builder, DefaultAllocator())); };
    add(DefaultAllocator().New<pipeline::TextureAssetBuilder>());
    add(DefaultAllocator().New<pipeline::StaticMeshAssetBuilder>());
    add(DefaultAllocator().New<pipeline::SkinnedMeshAssetBuilder>());
    add(DefaultAllocator().New<pipeline::MaterialAssetBuilder>());
    add(DefaultAllocator().New<pipeline::SkeletonAssetBuilder>());
    add(DefaultAllocator().New<pipeline::AnimationClipAssetBuilder>());
    add(DefaultAllocator().New<pipeline::ModelManifestAssetBuilder>());
    foundation::vfs::NativeFileSystem sourcesFs(project->SourcesRoot().AsView(), DefaultAllocator());
    foundation::vfs::NativeFileSystem cacheFs(project->CacheRoot().AsView(), DefaultAllocator());
    CookDriver driver(DefaultAllocator(), project->SourceDb(), project->CookedDb(), builders, &sourcesFs, &cacheFs);
    CookPlan plan = driver.Plan();
    CookStats stats = driver.Execute(plan);
    CHECK(stats.failed == 0u);
    {
        RefPtr<ISerializable> product = project->CookedDb().ReadObject(albedoInst->Id());
        auto* res = Cast<foundation::texture::TextureResource>(product.Get());
        REQUIRE(res != nullptr);
        CHECK(res->format == foundation::rhi::TextureFormat::BC1RGBAUnormSrgb);
        CHECK(res->mipLevels == 3u);
    }
    {
        RefPtr<ISerializable> product = project->CookedDb().ReadObject(normalInst->Id());
        auto* res = Cast<foundation::texture::TextureResource>(product.Get());
        REQUIRE(res != nullptr);
        CHECK(res->format == foundation::rhi::TextureFormat::RGBA8Unorm);
        CHECK(res->mipLevels == 3u);
    }
    project.Reset();
    (void)RemoveDirectoryRecursive(dir);

    // The editor's path defers the bulk writes to a worker: every queued write must succeed.
    // The PNG twins the glTF names but the package does not ship are warnings, not writes
    // (a failed write fails the whole import in the editor).
    REQUIRE(EditorProject::Create(DefaultAllocator(), dir, u8"P").IsOk());
    project = EditorProject::Open(DefaultAllocator(), dir);
    REQUIRE(static_cast<bool>(project));
    Array<pipeline::DeferredImportWrite> deferred;
    imported = importer.Import(PathJoin(src, u8"tri.gltf").AsView(),
                               pipeline::ImportContext{DefaultAllocator(), project->SourcesRoot()},
                               *project->SourceDb().RootGroup(), nullptr, nullptr, &deferred);
    REQUIRE(imported.HasValue());
    CHECK_FALSE(deferred.IsEmpty());
    for (pipeline::DeferredImportWrite& write : deferred)
    {
        CHECK_MESSAGE(write.Execute().IsOk(), write.Label());
    }
    CHECK(FileExists(PathJoin(dir, u8"Sources/tex/albedo.dds").AsView()));
    CHECK(FileExists(PathJoin(dir, u8"Sources/tex/normal.dds").AsView()));
    CHECK(FileExists(PathJoin(dir, u8"Sources/tri.bin").AsView()));
    CHECK_FALSE(FileExists(PathJoin(dir, u8"Sources/tex/albedo.png").AsView()));
    project.Reset();
    (void)RemoveDirectoryRecursive(dir);
    (void)RemoveDirectoryRecursive(src);
}


TEST_CASE("model-import: with a prepared model the geometry writes are LAZY - produced on the flush, byte-identical to the inline import")
{
    // The editor's path: PrepareOnWorker loads the model, Import queues the mesh writes in
    // microseconds (no conversion, no LOD chain, no serialization on the main thread), and the
    // deferred flush produces the geometry on the worker while the prepared model stays alive.
    using namespace editor;
    pipeline::RegisterModelManifestAsset();
    pipeline::RegisterTextureAsset();
    pipeline::RegisterMeshAssets();
    pipeline::RegisterMaterialAsset();
    pipeline::RegisterAnimationAssets();
    const auto freshProject = [](StringView dir) -> UniquePtr<EditorProject>
    {
        (void)RemoveDirectoryRecursive(dir);
        REQUIRE(EditorProject::Create(DefaultAllocator(), dir, u8"P").IsOk());
        UniquePtr<EditorProject> project = EditorProject::Open(DefaultAllocator(), dir);
        REQUIRE(static_cast<bool>(project));
        return project;
    };
    const auto geometryBytes = [](const foundation::content::Instance& mesh) -> Array<byte>
    {
        UniquePtr<IStream> stream = mesh.ReadData(pipeline::kMeshGeometryStreamName);
        Array<byte> bytes;
        REQUIRE(stream.Get() != nullptr);
        const i64 size = stream->Size();
        REQUIRE(size > 0);
        bytes.Resize(static_cast<usize>(size));
        REQUIRE(stream->Read(bytes.Data(), static_cast<u64>(size)) == static_cast<u64>(size));
        return bytes;
    };
    pipeline::ModelFileImporter importer;

    // The inline reference: no prepared model, no deferred writes.
    const StringView inlineDir = u8"scratch_gltf_lazy_inline";
    UniquePtr<EditorProject> inlineProject = freshProject(inlineDir);
    Result<foundation::content::Instance*> inlineImport = importer.Import(
        MiFoxNested().AsView(),
        pipeline::ImportContext{DefaultAllocator(), inlineProject->SourcesRoot()},
        *inlineProject->SourceDb().RootGroup(), nullptr, nullptr, nullptr);
    REQUIRE(inlineImport.HasValue());
    foundation::content::Instance* inlineMesh = nullptr;
    for (foundation::content::Instance* candidate : inlineImport.Value()->OwningGroup().Instances())
    {
        if (candidate->TypeName() == StringView(u8"SkinnedMeshAsset") ||
            candidate->TypeName() == StringView(u8"StaticMeshAsset"))
        {
            inlineMesh = candidate;
            break;
        }
    }
    REQUIRE(inlineMesh != nullptr);
    const Array<byte> reference = geometryBytes(*inlineMesh);

    // The editor's path.
    const StringView lazyDir = u8"scratch_gltf_lazy_deferred";
    UniquePtr<EditorProject> lazyProject = freshProject(lazyDir);
    Array<pipeline::DeferredImportWrite> deferred;
    RefPtr<Object> prepared = importer.PrepareOnWorker(MiFoxNested().AsView(), DefaultAllocator());
    REQUIRE(prepared.Get() != nullptr);
    Result<foundation::content::Instance*> lazyImport = importer.Import(
        MiFoxNested().AsView(),
        pipeline::ImportContext{DefaultAllocator(), lazyProject->SourcesRoot()},
        *lazyProject->SourceDb().RootGroup(), nullptr, prepared.Get(), &deferred);
    REQUIRE(lazyImport.HasValue());
    usize lazyGeometryWrites = 0;
    for (const pipeline::DeferredImportWrite& write : deferred)
    {
        if (write.streamName.AsView() == pipeline::kMeshGeometryStreamName)
        {
            ++lazyGeometryWrites;
            CHECK(static_cast<bool>(write.produce)); // the work is parked on the worker, and
            CHECK(write.owned.IsEmpty());            // nothing was serialized on this thread
        }
    }
    CHECK(lazyGeometryWrites >= 1u);
    for (pipeline::DeferredImportWrite& write : deferred)
    {
        CHECK_MESSAGE(write.Execute().IsOk(), write.Label());
    }
    foundation::content::Instance* lazyMesh =
        lazyImport.Value()->OwningGroup().GetInstance(inlineMesh->Name());
    REQUIRE(lazyMesh != nullptr);
    const Array<byte> produced = geometryBytes(*lazyMesh);
    REQUIRE(produced.Size() == reference.Size());
    CHECK(MemCompare(produced.Data(), reference.Data(), reference.Size()) == 0);
    // The envelope was written after the geometry was produced: it reads back as a mesh asset.
    RefPtr<ISerializable> envelope = lazyMesh->ReadObject();
    CHECK(envelope.Get() != nullptr);

    inlineProject.Reset();
    lazyProject.Reset();
    (void)RemoveDirectoryRecursive(inlineDir);
    (void)RemoveDirectoryRecursive(lazyDir);
}

// A model whose material is named like the model itself (the platformer kit's Gem_Blue.gltf has a
// material "Gem_Blue"): the material takes a suffix, and the manifest keeps the file's name and
// its own instance, rather than being written into the material's.
TEST_CASE("model-import: a material named like the model leaves the manifest its own instance")
{
    using namespace editor;

    pipeline::RegisterModelManifestAsset();
    pipeline::RegisterTextureAsset();
    pipeline::RegisterMeshAssets();
    pipeline::RegisterMaterialAsset();
    pipeline::RegisterAnimationAssets();

    const String dir(u8"scratch_gltf_samename_project");
    (void)RemoveDirectoryRecursive(dir.AsView());
    (void)RemoveDirectoryRecursive(u8"scratch_gltf_samename_src");
    REQUIRE(CreateDirectories(u8"scratch_gltf_samename_src"));
    const String source = PathJoin(u8"scratch_gltf_samename_src", u8"Gem.gltf");
    const StringView gltf =
        u8R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],
"nodes":[{"name":"Gem","mesh":0}],
"meshes":[{"name":"Gem","primitives":[{"attributes":{"POSITION":0},"material":0}]}],
"materials":[{"name":"Gem","pbrMetallicRoughness":{"baseColorFactor":[0.1,0.5,0.7,1]}}],
"buffers":[{"byteLength":36,"uri":"data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAA"}],
"bufferViews":[{"buffer":0,"byteLength":36}],
"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]}]})";
    REQUIRE(WriteFile(source.AsView(), Span<const byte>(reinterpret_cast<const byte*>(gltf.Data()), gltf.Size()))
                .IsOk());
    REQUIRE(EditorProject::Create(DefaultAllocator(), dir.AsView(), u8"P").IsOk());
    UniquePtr<EditorProject> project = EditorProject::Open(DefaultAllocator(), dir.AsView());
    REQUIRE(static_cast<bool>(project));

    pipeline::ModelFileImporter importer;
    Result<foundation::content::Instance*> imported =
        importer.Import(source.AsView(), pipeline::ImportContext{DefaultAllocator(), project->SourcesRoot()},
                        *project->SourceDb().RootGroup(), nullptr, nullptr, nullptr);
    REQUIRE(imported.HasValue());
    REQUIRE(imported.Value() != nullptr);
    CHECK(imported.Value()->Name() == u8"Gem");
    RefPtr<ISerializable> object = imported.Value()->ReadObject();
    auto* manifest = Cast<pipeline::ModelManifestAsset>(object.Get());
    REQUIRE(manifest != nullptr);
    REQUIRE(manifest->manifest.materialGuids.Size() == 1u);

    // The material is its own instance, a MaterialAsset under a suffixed name.
    foundation::content::Instance* material =
        project->SourceDb().GetInstance(manifest->manifest.materialGuids[0]);
    REQUIRE(material != nullptr);
    CHECK(material != imported.Value());
    CHECK(material->Name() == u8"Gem.2");
    RefPtr<ISerializable> materialObject = material->ReadObject();
    CHECK(Cast<pipeline::MaterialAsset>(materialObject.Get()) != nullptr);

    project = nullptr;
    (void)RemoveDirectoryRecursive(dir.AsView());
    (void)RemoveDirectoryRecursive(u8"scratch_gltf_samename_src");
}

TEST_CASE("model-import: the skeleton's parent node is the skin's root joint's parent")
{
    // The prefab puts skinned meshes at identity under this node,
    // so it must be the node the joints hang from: Blender's armature object, not a joint.
    model::Model m;
    auto bone = [&](StringView name, i32 parent)
    {
        auto* b = new model::ModelBone();
        b->setName(name);
        b->parentIndex = parent;
        return m.addBone(b);
    };
    const i32 scene = bone(u8"Scene", -1);
    const i32 armature = bone(u8"Armature", scene);
    const i32 hips = bone(u8"Hips", armature);
    const i32 spine = bone(u8"Spine", hips);
    (void)bone(u8"Body", armature);

    // Joints listed child first: the root is found by its parent, not by its place in the list.
    model::ModelSkin skin;
    skin.addJoint(spine, Float4x4::Identity());
    skin.addJoint(hips, Float4x4::Identity());
    CHECK(pipeline::SkeletonParentNode(m, pipeline::SkeletonJoints(m, skin).AsSpan()) == armature);

    // A root joint at the top of the file has no parent node: the scene root (-1).
    model::ModelSkin top;
    top.addJoint(scene, Float4x4::Identity());
    top.addJoint(armature, Float4x4::Identity());
    CHECK(pipeline::SkeletonParentNode(m, pipeline::SkeletonJoints(m, top).AsSpan()) == -1);

    // No joints: unknown (-2), and the prefab keeps the file's placement.
    model::ModelSkin empty;
    CHECK(pipeline::SkeletonParentNode(m, pipeline::SkeletonJoints(m, empty).AsSpan()) == -2);
}

TEST_CASE("model-import: the node an unskinned mesh hangs under joins the skeleton, its animation played")
{
    // 3ds Max's Biped animates a weapon's prop node with the hand; the file does not make it a
    // bone and no vertex is bound to it. Left outside the skeleton, its channels would be dropped
    // and the weapon would follow the pelvis.
    model::Model m;
    auto bone = [&](StringView name, i32 parent, bool isBone, i32 mesh = -1)
    {
        auto* b = new model::ModelBone();
        b->setName(name);
        b->parentIndex = parent;
        b->isSkeletonBone = isBone;
        b->meshIndex = mesh;
        return m.addBone(b);
    };
    for (usize i = 0; i < 4; ++i)
        m.addMesh(new model::ModelMesh()); // no joints or weights: unskinned
    const i32 root = bone(u8"100001", -1, false);
    const i32 bip = bone(u8"Bip001", root, true);
    const i32 hand = bone(u8"Bip001 R Hand", bip, true);
    const i32 prop = bone(u8"Bip001 Prop1", bip, false); // not a file bone
    const i32 grip = bone(u8"Grip", prop, false);        // the node the weapon hangs under
    bone(u8"Weapon", grip, false, 1);
    const i32 apart = bone(u8"Stand", root, false); // a node the skeleton does not reach
    bone(u8"Banner", apart, false, 2);
    bone(u8"Hilt", hand, false, 3); // under a joint already: nothing to add
    model::ModelSkin skin;
    skin.addJoint(bip, Float4x4::Identity());
    skin.addJoint(hand, Float4x4::Identity());

    model::ModelAnimation swing;
    swing.duration = 1.0f;
    auto* lift = new model::AnimationChannel();
    lift->targetBone = prop;
    lift->path = model::AnimationPath::Translation;
    lift->addKeyframe(0.0f, Float4{0.0f, 0.0f, 0.0f, 0.0f});
    lift->addKeyframe(1.0f, Float4{0.0f, 1.0f, 0.0f, 0.0f});
    swing.addChannel(lift);

    const Array<i32> joints = pipeline::SkeletonJoints(m, skin);
    REQUIRE(joints.Size() == 4u); // the skin's two, then the prop and the grip; not the stand
    CHECK(joints[0] == bip);
    CHECK(joints[1] == hand);
    CHECK(joints[2] == prop);
    CHECK(joints[3] == grip);
    const HashMap<i32, i32> boneToJoint = pipeline::BuildBoneToJoint(joints.AsSpan());
    foundation::animation::SkeletonSource skeleton;
    pipeline::SkeletonSourceFromModel(m, skin, joints.AsSpan(), boneToJoint, skeleton);
    CHECK(skeleton.boneNames[2] == String(u8"Bip001 Prop1"));
    CHECK(skeleton.parentIndices[2] == 0); // under the pelvis's joint
    foundation::animation::AnimationClipSource clip;
    pipeline::AnimationClipSourceFromModel(swing, boneToJoint, u8"swing", clip, pipeline::SkeletonParentNode(m, joints.AsSpan()));
    REQUIRE(clip.trackBone.Size() == 1u);
    CHECK(clip.trackBone[0] == 2); // the prop's swing is a track of the pose
}

TEST_CASE("model-import: a bone the skin's joints hang from joins the skeleton, its animation played; an empty one does not")
{
    // 3ds Max hangs a skeleton from a Dummy helper bone with no vertices of its own and animates it
    // with the pose (a creature bobbing as it idles). Left outside the skeleton, as Blender's
    // armature object is, the bob would be a model track the pose never plays.
    for (const bool helperIsBone : {true, false})
    {
        model::Model m;
        auto bone = [&](StringView name, i32 parent, bool isBone)
        {
            auto* b = new model::ModelBone();
            b->setName(name);
            b->parentIndex = parent;
            b->isSkeletonBone = isBone;
            return m.addBone(b);
        };
        const i32 root = bone(u8"300024", -1, false);
        const i32 dummy = bone(u8"Dummy001", root, helperIsBone);
        const i32 spine = bone(u8"Bone001", dummy, true);
        const i32 arm = bone(u8"Bone002", spine, true);
        model::ModelSkin skin;
        skin.addJoint(spine, Float4x4::Identity());
        skin.addJoint(arm, Float4x4::Identity());

        model::ModelAnimation idle;
        idle.duration = 1.0f;
        auto* bob = new model::AnimationChannel();
        bob->targetBone = dummy;
        bob->path = model::AnimationPath::Translation;
        bob->addKeyframe(0.0f, Float4{0.0f, 1.0f, 0.0f, 0.0f});
        bob->addKeyframe(1.0f, Float4{0.0f, 1.6f, 0.0f, 0.0f});
        idle.addChannel(bob);
        auto* wave = new model::AnimationChannel();
        wave->targetBone = arm;
        wave->path = model::AnimationPath::Rotation;
        wave->addKeyframe(0.0f, Float4{0.0f, 0.0f, 0.0f, 1.0f});
        idle.addChannel(wave);

        const Array<i32> joints = pipeline::SkeletonJoints(m, skin);
        const HashMap<i32, i32> boneToJoint = pipeline::BuildBoneToJoint(joints.AsSpan());
        foundation::animation::SkeletonSource skeleton;
        pipeline::SkeletonSourceFromModel(m, skin, joints.AsSpan(), boneToJoint, skeleton);
        const i32 hangsFrom = pipeline::SkeletonParentNode(m, joints.AsSpan());
        foundation::animation::AnimationClipSource clip;
        pipeline::AnimationClipSourceFromModel(idle, boneToJoint, u8"idle", clip, hangsFrom);
        REQUIRE(clip.trackBone.Size() == 2u);
        if (helperIsBone)
        {
            // Appended after the skin's joints (no vertex's joint index moves), the skin's root
            // joint under it, the mesh's placement node above it, and its bob a track of the pose.
            REQUIRE(joints.Size() == 3u);
            CHECK(joints[2] == dummy);
            CHECK(skeleton.boneNames[2] == String(u8"Dummy001"));
            CHECK(skeleton.parentIndices[0] == 2);
            CHECK(skeleton.parentIndices[2] == -1);
            CHECK(hangsFrom == root);
            CHECK(clip.trackBone[0] == 2);
        }
        else
        {
            // An empty node: the node the skeleton hangs from, its channel the clip's model track.
            CHECK(joints.Size() == 2u);
            CHECK(skeleton.parentIndices[0] == -1);
            CHECK(hangsFrom == dummy);
            CHECK(clip.trackBone[0] == -1);
        }
        CHECK(clip.trackBone[1] == 1); // the arm, wherever the helper went
    }
}

TEST_CASE("model-import: a clip keeps the armature's channels, its skeleton and rest, and a re-import keeps its root motion")
{
    // The armature node (the skeleton's parent) animated in the file keeps its
    // channels as model tracks (bone -1), the clip names its skeleton and the armature's rest for
    // the root motion cook, and root motion settings authored on the clip survive a re-import.
    using namespace editor;
    pipeline::RegisterModelManifestAsset();
    pipeline::RegisterAnimationAssets();
    model::RegisterModelResourceTypes();

    const StringView dir = u8"scratch_model_rootmotion_project";
    for (StringView f : {u8"Content/walker/Walk.xasset", u8"Content/walker/walker.xasset",
                         u8"Content/walker/skeleton.xasset", u8"Content/walker/Run.xasset"})
    {
        FileDelete(PathJoin(dir, f));
    }
    (void)RemoveDirectory(PathJoin(dir, u8"Content/walker"));
    FileDelete(PathJoin(dir, u8"Sources/walker.glb"));
    for (StringView sub : {u8"Content", u8"Cooked", u8"Sources", u8".cache", u8"Editor"})
    {
        (void)RemoveDirectory(PathJoin(dir, sub));
    }
    FileDelete(PathJoin(dir, u8"Project.xml"));
    (void)RemoveDirectory(dir);
    REQUIRE(EditorProject::Create(DefaultAllocator(), dir, u8"P").IsOk());
    UniquePtr<EditorProject> project = EditorProject::Open(DefaultAllocator(), dir);
    REQUIRE(static_cast<bool>(project));
    const String fakeSource = PathJoin(dir, u8"walker.glb");
    const byte junk[4] = {byte{1}, byte{2}, byte{3}, byte{4}};
    REQUIRE(WriteFile(fakeSource.AsView(), Span<const byte>(junk, 4)).IsOk());

    const Quaternion turn = Quaternion::FromAxisAngle(Float3{0, 1, 0}, 0.5f);
    const auto prepare = [&](bool withRun)
    {
        auto prepared = MakeRef<pipeline::LoadedModel>(DefaultAllocator());
        auto* armature = new model::ModelBone();
        armature->setName(u8"Armature");
        armature->rotation = turn;
        (void)prepared->model.addBone(armature);
        auto* hips = new model::ModelBone();
        hips->setName(u8"Hips");
        hips->parentIndex = 0;
        (void)prepared->model.addBone(hips);
        auto* skin = new model::ModelSkin();
        skin->setName(u8"skeleton");
        skin->addJoint(1, Float4x4::Identity());
        (void)prepared->model.addSkin(skin);
        for (StringView name : {StringView(u8"Walk"), StringView(u8"Run")})
        {
            if (name == StringView(u8"Run") && !withRun)
            {
                continue;
            }
            auto* animation = new model::ModelAnimation();
            animation->setName(name);
            animation->duration = 1.0f;
            auto* onHips = new model::AnimationChannel();
            onHips->targetBone = 1;
            onHips->path = model::AnimationPath::Translation;
            onHips->addKeyframe(0.0f, Float4{0, 1, 0, 0});
            onHips->addKeyframe(1.0f, Float4{0, 1, 0.1f, 0});
            animation->addChannel(onHips);
            auto* onArmature = new model::AnimationChannel();
            onArmature->targetBone = 0;
            onArmature->path = model::AnimationPath::Translation;
            onArmature->addKeyframe(0.0f, Float4{0, 0, 0, 0});
            onArmature->addKeyframe(1.0f, Float4{0, 0, 2, 0});
            animation->addChannel(onArmature);
            (void)prepared->model.addAnimation(animation);
        }
        return prepared;
    };

    pipeline::ModelFileImporter importer;
    auto prepared = prepare(false);
    Result<foundation::content::Instance*> imported = importer.Import(
        fakeSource.AsView(), pipeline::ImportContext{DefaultAllocator(), project->SourcesRoot()},
        *project->SourceDb().RootGroup(), nullptr, prepared.Get(), nullptr);
    REQUIRE(imported.HasValue());
    foundation::content::Group& group = imported.Value()->OwningGroup();
    const auto readClip = [&](StringView name) -> RefPtr<ISerializable>
    {
        foundation::content::Instance* clip = group.GetInstance(name);
        REQUIRE(clip != nullptr);
        return clip->ReadObject();
    };
    RefPtr<ISerializable> walkObject = readClip(u8"Walk");
    auto* walk = Cast<pipeline::AnimationClipAsset>(walkObject.Get());
    REQUIRE(walk != nullptr);
    usize modelTracks = 0;
    for (const i32 bone : walk->source.trackBone)
    {
        modelTracks += bone < 0 ? 1u : 0u;
    }
    CHECK(modelTracks == 1u);          // the armature's translation
    CHECK(walk->source.trackBone.Size() == 2u);
    CHECK_FALSE(walk->skeleton.IsNil());
    CHECK(group.GetInstance(u8"skeleton") != nullptr);
    CHECK(walk->skeleton == group.GetInstance(u8"skeleton")->Id());
    CHECK(std::abs(Dot(walk->modelRest.rotation, turn)) > 1.0f - 1.0e-5f);
    CHECK_FALSE(walk->source.rootMotion.Any()); // off unless asked

    // Authored on the clip, kept by a re-import; a clip new in the file takes the import option.
    walk->source.rootMotion.horizontal = true;
    walk->source.rootMotion.rootBone = String(u8"Hips");
    REQUIRE(group.GetInstance(u8"Walk")->WriteObject(*walk).IsOk());
    pipeline::ModelImportOptions options;
    options.rootMotion = true;
    auto again = prepare(true);
    Result<foundation::content::Instance*> reimported = importer.Import(
        fakeSource.AsView(), pipeline::ImportContext{DefaultAllocator(), project->SourcesRoot()},
        *project->SourceDb().RootGroup(), &options, again.Get(), nullptr);
    REQUIRE(reimported.HasValue());
    RefPtr<ISerializable> walkAgain = readClip(u8"Walk");
    auto* kept = Cast<pipeline::AnimationClipAsset>(walkAgain.Get());
    REQUIRE(kept != nullptr);
    CHECK(kept->source.rootMotion.horizontal);
    CHECK_FALSE(kept->source.rootMotion.yaw); // the clip's, not the option's
    CHECK(kept->source.rootMotion.rootBone == StringView(u8"Hips"));
    RefPtr<ISerializable> runObject = readClip(u8"Run");
    auto* run = Cast<pipeline::AnimationClipAsset>(runObject.Get());
    REQUIRE(run != nullptr);
    CHECK(run->source.rootMotion.horizontal);
    CHECK(run->source.rootMotion.yaw);
}

// Each material made on the built-in its shading model names, as its file authored it, by the
// one function both cooks make materials with.
TEST_CASE("model import: a material is made on the built-in its shading model names, its maps wired")
{
    model::Model mdl;
    const auto add = [&](model::ShadingModel shading) -> model::ModelMaterial&
    {
        auto* material = new model::ModelMaterial();
        material->shadingModel = shading;
        material->baseColorTextureIndex = 0;
        material->normalTextureIndex = 1;
        (void)mdl.addMaterial(material);
        return *material;
    };
    model::ModelMaterial& phong = add(model::ShadingModel::Phong);
    phong.specularColor = Float3{0.5f, 0.25f, 0.125f};
    phong.specularTextureIndex = 2;
    phong.shininess = 32.0f;
    model::ModelMaterial& lambert = add(model::ShadingModel::Lambert);
    lambert.specularColor = Float3{0.5f, 0.5f, 0.5f}; // a Lambert material has no highlight, whatever it says
    lambert.specularTextureIndex = 2;
    (void)add(model::ShadingModel::Unlit);
    model::ModelMaterial& pbr = add(model::ShadingModel::Pbr);
    pbr.separateRoughnessTextureIndex = 3;
    pbr.occlusionTextureIndex = 4;

    Array<Guid> textures;
    for (u64 t = 1; t <= 5; ++t)
        textures.PushBack(Guid{t, t});
    i32 bakes = 0;
    const Function<Guid(i32, i32)> packed = [&](i32, i32)
    {
        ++bakes;
        return Guid{9ull, 9ull};
    };
    const auto build = [&](usize i, pipeline::MaterialAsset& asset) { pipeline::BuildModelMaterial(mdl, *mdl.materials()[i], u8"m", textures, packed, asset); };
    const auto slots = [](const pipeline::MaterialAsset& asset)
    {
        String out;
        for (const String& slot : asset.source.textureSlots)
        {
            if (!out.IsEmpty())
                out += u8" ";
            out += slot.AsView();
        }
        return out;
    };
    const auto uniform = [](const pipeline::MaterialAsset& asset, StringView name) -> Float4
    {
        Float4 value{};
        for (usize i = 0; i < asset.source.propertyNames.Size(); ++i)
            if (asset.source.propertyNames[i].AsView() == name)
                MemCopy(&value, asset.source.uniformDefaults.Data() + asset.source.propertyOffsets[i],
                        Min<usize>(sizeof(value), asset.source.propertySizes[i]));
        return value;
    };

    pipeline::MaterialAsset onPhong;
    build(0, onPhong);
    CHECK(onPhong.source.shaderName == u8"phong");
    CHECK(slots(onPhong) == String(u8"AlbedoMap NormalMap SpecularMap"));
    CHECK(uniform(onPhong, u8"SpecularColor").y == doctest::Approx(0.25f));
    CHECK(uniform(onPhong, u8"Shininess").x == doctest::Approx(32.0f));

    pipeline::MaterialAsset onLambert;
    build(1, onLambert);
    CHECK(onLambert.source.shaderName == u8"phong");
    CHECK(slots(onLambert) == String(u8"AlbedoMap NormalMap")); // no highlight, so no specular map
    CHECK(uniform(onLambert, u8"SpecularColor").x == 0.0f);

    pipeline::MaterialAsset onUnlit;
    build(2, onUnlit);
    CHECK(onUnlit.source.shaderName == u8"unlit");
    CHECK(slots(onUnlit) == String(u8"AlbedoMap"));

    pipeline::MaterialAsset onPbr;
    build(3, onPbr);
    CHECK(onPbr.source.shaderName == u8"forward");
    CHECK(slots(onPbr) == String(u8"AlbedoMap NormalMap OcclusionMap MetallicRoughnessMap"));
    CHECK(bakes == 1); // its separate rough map baked into the packed one by the caller
}

TEST_CASE("model-cook: a mesh, a texture and a material of one name all cook (3ds Max exports name them alike)")
{
    // The direct cook makes the names of meshes, clips and the skeleton unique: a mesh named
    // like its texture still gets its instance, and the model comes with its body.
    model::RegisterModelResourceTypes();
    model::gltf::GltfLoader gltf;
    model::io::registerLoader(&gltf);
    model::Model m;
    const String duck = MiDuck();
    const model::ModelLoadResult loaded = model::io::loadModel(duck.AsView(), m);
    model::io::unregisterLoader(&gltf);
    REQUIRE(loaded == model::ModelLoadResult::Ok);
    REQUIRE(m.meshes().Size() > 0u);
    REQUIRE(m.textures().Size() > 0u);
    const String shared(m.meshes()[0]->name());
    m.textures()[0]->setName(shared.AsView());
    REQUIRE(m.materials().Size() > 0u);
    m.materials()[0]->setName(shared.AsView());

    vfs::NativeFileSystem mount(u8"scratch_modelcook_names_db", DefaultAllocator());
    (void)RemoveDirectoryRecursive(u8"scratch_modelcook_names_db");
    content::ContentDatabase db(DefaultAllocator(), mount, BinarySerializerFactory(), u8".rasset");
    Guid modelGuid;
    REQUIRE(pipeline::CookModel(m, db, u8"Duck", modelGuid).IsOk());
    resource::ResourceManager manager(DefaultAllocator(), db);
    geometry::StaticMeshFactory meshFactory(DefaultAllocator());
    model::ModelFactory modelFactory;
    manager.AddFactory(&meshFactory);
    manager.AddFactory(&modelFactory);
    resource::Proxy<model::ModelResource> cooked = manager.Bind<model::ModelResource>(modelGuid);
    REQUIRE(cooked);
    REQUIRE(cooked->meshes.Size() == m.meshes().Size());
    for (const RefPtr<geometry::StaticMesh>& mesh : cooked->meshes)
        CHECK(mesh.Get() != nullptr);
}

// A model claims its folder by the import's clash rule (pipeline::FindImportClash): the same file
// again is a re-import in place (its manifest's guid kept); another folder's file of the same name
// is refused, or kept beside in the next folder ("character-oozi.2") with its own copy under
// Sources/, the first model's untouched.
TEST_CASE("model import: the same file re-imports in place; another file of its name is refused or kept beside")
{
    pipeline::RegisterModelManifestAsset(); // the clash check reads the manifest back
    pipeline::RegisterTextureAsset();
    pipeline::RegisterMeshAssets();
    pipeline::RegisterMaterialAsset();
    pipeline::RegisterAnimationAssets();
    const String dir = PathJoin(GetCurrentDirectory().AsView(), u8"scratch_model_clash");
    (void)RemoveDirectoryRecursive(dir.AsView());
    REQUIRE(editor::EditorProject::Create(DefaultAllocator(), dir.AsView(), u8"P").IsOk());
    UniquePtr<editor::EditorProject> project = editor::EditorProject::Open(DefaultAllocator(), dir.AsView());
    REQUIRE(static_cast<bool>(project));
    // Another folder's character-oozi.glb: another file (a different model) of the same name.
    const String elsewhere = PathJoin(dir.AsView(), u8"elsewhere");
    REQUIRE(CreateDirectories(elsewhere.AsView()));
    const String other = PathJoin(elsewhere.AsView(), u8"character-oozi.glb");
    REQUIRE(FileCopyPreserving(MiPath(u8"Tests/Models/KenneyPlatformerCharacters/GLB/character-oobi.glb").AsView(), other.AsView()));

    pipeline::ModelFileImporter modelImporter;
    pipeline::IFileImporter& importer = modelImporter;
    pipeline::ImportContext context{DefaultAllocator(), project->SourcesRoot()};
    context.ledgerPath = pipeline::ImportLedgerPath(project->CacheRoot().AsView());
    foundation::content::Group& root = *project->SourceDb().RootGroup();

    Result<foundation::content::Instance*> first = importer.Import(MiGlb().AsView(), context, root);
    REQUIRE(first.HasValue());
    const Guid manifest = first.Value()->Id();
    CHECK(importer.FindClash(root, MiGlb().AsView(), {}, context.ledgerPath.AsView()).kind == pipeline::ImportClashKind::Reimport);
    Result<foundation::content::Instance*> again = importer.Import(MiGlb().AsView(), context, root);
    REQUIRE(again.HasValue());
    CHECK(again.Value()->Id() == manifest);
    CHECK(root.GetGroup(u8"character-oozi.2") == nullptr);

    CHECK(importer.FindClash(root, other.AsView(), {}, context.ledgerPath.AsView()).kind == pipeline::ImportClashKind::SameType);
    CHECK_FALSE(importer.Import(other.AsView(), context, root).HasValue());
    context.onConflict = pipeline::ImportConflictChoice::KeepBoth;
    Result<foundation::content::Instance*> beside = importer.Import(other.AsView(), context, root);
    REQUIRE(beside.HasValue());
    CHECK(beside.Value()->Id() != manifest);
    foundation::content::Group* second = root.GetGroup(u8"character-oozi.2");
    REQUIRE(second != nullptr);
    CHECK(&beside.Value()->OwningGroup() == second);
    RefPtr<ISerializable> object = beside.Value()->ReadObject();
    CHECK(Cast<pipeline::Asset>(object.Get())->fileName.View() == StringView(u8"character-oozi.2.glb"));
    RefPtr<ISerializable> firstObject = project->SourceDb().GetInstance(manifest)->ReadObject();
    CHECK(Cast<pipeline::Asset>(firstObject.Get())->fileName.View() == StringView(u8"character-oozi.glb"));
    project.Reset();
    (void)RemoveDirectoryRecursive(dir.AsView());
}
