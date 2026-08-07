// Draconic::ModelImporter tests - load a real glTF, cook it through the importer into a
// content DB, then bind the cooked ModelResource back through the resource manager and
// verify the whole convert -> cook -> bind chain (manifest nodes + resolved meshes).

#include "Draconic.Foundation/Prelude.h"
#include <doctest/doctest.h>
#include <initializer_list>

import draconic.foundation;
import draconic.vfs;
import draconic.content;
import draconic.resource;
import draconic.geometry;
import draconic.geometry.resource;
import draconic.animation;
import draconic.animation.resource;
import draconic.model;
import draconic.model.io;
import draconic.modelimporter;
import draconic.physics.editor;
import draconic.editor;
import draconic.editor.core;
import draconic.editor.cook;
import draconic.texture.editor;
import draconic.geometry.editor;
import draconic.materials.editor;
import draconic.animation.editor;
import draconic.materials;
import draconic.texture;
import draconic.texture.resource;
import draconic.rhi;
import draconic.rhi.null;
import draconic.materials.resource;

using namespace draconic::foundation;
namespace vfs = draconic::vfs;
namespace content = draconic::content;
namespace resource = draconic::resource;
namespace geometry = draconic::geometry;
namespace model = draconic::model;
namespace modelimporter = draconic::modelimporter;

#ifndef DRACONIC_MI_TEST_DUCK
#define DRACONIC_MI_TEST_DUCK ""
#endif
#ifndef DRACONIC_MI_TEST_FOX
#define DRACONIC_MI_TEST_FOX ""
#endif

TEST_CASE("import glTF -> cooked ModelResource round-trips through the resource system")
{
    const StringView duck(reinterpret_cast<const utf8char*>(DRACONIC_MI_TEST_DUCK));
    if (duck.IsEmpty())
    {
        return;
    } // path not configured (skip)

    model::RegisterModelResourceTypes(); // make the cooked types deserializable

    vfs::NativeFileSystem mount(u8"draconic_modelimporter_test_db");
    content::ContentDatabase db(mount, draconic::foundation::BinarySerializerFactory(), u8".rasset");

    // Cook the model file into the DB; get back the manifest (ModelResource) Guid.
    Guid modelGuid;
    const model::ModelLoadResult r = modelimporter::LoadAndCook(duck, db, u8"Duck", modelGuid);
    REQUIRE(r == model::ModelLoadResult::Ok);
    REQUIRE_FALSE(modelGuid.IsNil());

    // Bind the composite model: ModelFactory resolves its meshes via StaticMeshFactory.
    resource::ResourceManager manager(db);
    geometry::StaticMeshFactory meshFactory;
    model::ModelFactory modelFactory;
    manager.AddFactory(&meshFactory);
    manager.AddFactory(&modelFactory);

    resource::Proxy<model::ModelResource> model = manager.Bind<model::ModelResource>(modelGuid);
    REQUIRE(model);
    CHECK(model->nodes.Size() > 0);
    CHECK(model->meshes.Size() > 0);

    // At least one node references a mesh, and that mesh resolved with real geometry.
    bool sawMesh = false;
    for (const modelimporter::ModelNode& n : model->nodes)
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
    const StringView fox(reinterpret_cast<const utf8char*>(DRACONIC_MI_TEST_FOX));
    if (fox.IsEmpty())
    {
        return;
    }

    model::RegisterModelResourceTypes();

    vfs::NativeFileSystem mount(u8"draconic_modelimporter_fox_db");
    content::ContentDatabase db(mount, draconic::foundation::BinarySerializerFactory(), u8".rasset");

    Guid modelGuid;
    REQUIRE(modelimporter::LoadAndCook(fox, db, u8"Fox", modelGuid) == model::ModelLoadResult::Ok);

    resource::ResourceManager manager(db);
    geometry::StaticMeshFactory meshFactory;
    geometry::SkinnedMeshFactory skinnedFactory;
    model::ModelFactory modelFactory;
    draconic::animation::SkeletonFactory skeletonFactory;
    draconic::animation::AnimationClipFactory clipFactory;
    manager.AddFactory(&meshFactory);
    manager.AddFactory(&skinnedFactory);
    manager.AddFactory(&modelFactory);
    manager.AddFactory(&skeletonFactory);
    manager.AddFactory(&clipFactory);

    resource::Proxy<model::ModelResource> model = manager.Bind<model::ModelResource>(modelGuid);
    REQUIRE(model);

    // The Fox is skinned + animated: a resolved skeleton with bones + animation clips.
    REQUIRE(model->skeleton);
    CHECK(model->skeleton->BoneCount() > 0);
    CHECK(model->animations.Size() > 0);
    for (const auto& clip : model->animations)
    {
        REQUIRE(clip);
        CHECK(clip->duration > 0.0f);
    }

    // The mesh is flagged skinned + resolved as a SkinnedMesh (skin stream present).
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

// === Source-side file import (editor pipeline): GLB fan-out + full incremental cook ===

TEST_CASE("model-import: GLB fans out into source assets and cooks through the driver")
{
    using namespace draconic::editor;
    namespace modelimporter = draconic::modelimporter;

    // Types the fan-out creates + their builders.
    modelimporter::RegisterModelManifestAsset();
    draconic::texture::RegisterTextureAsset();
    draconic::geometry::RegisterMeshAssets();
    draconic::materials::RegisterMaterialAsset();
    draconic::animation::RegisterAnimationAssets();
    // Product types too (the test reads a cooked product back).
    GlobalTypeRegistry().Register(draconic::geometry::StaticMeshSource::StaticType());
    GlobalTypeRegistry().Register(draconic::geometry::SkinnedMeshSource::StaticType());
    RegisterSerializable<draconic::geometry::StaticMeshSource>();
    RegisterSerializable<draconic::geometry::SkinnedMeshSource>();

    const StringView dir = u8"draconic_model_import_project";
    auto cleanTree = [&]()
    {
        // Recursive best-effort cleanup of Content/Cooked/Sources/.cache trees.
        for (StringView sub : {u8"Content", u8"Cooked", u8"Sources", u8".cache"})
        {
            draconic::vfs::NativeFileSystem fs(PathJoin(dir, sub).AsView());
            Array<draconic::vfs::DirEntry> tops;
            if (fs.AsEnumerable()->Enumerate(u8"", tops).IsOk())
            {
                for (const auto& top : tops)
                {
                    if (!top.isDirectory)
                    {
                        (void)fs.AsWritable()->Delete(top.name.AsView());
                        continue;
                    }
                    Array<draconic::vfs::DirEntry> inner;
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
    REQUIRE(EditorProject::Create(dir, u8"P").IsOk());
    UniquePtr<EditorProject> project = EditorProject::Open(dir);
    REQUIRE(static_cast<bool>(project));

    // Import the Kenney character GLB (skinned: skeleton + clips expected).
    modelimporter::ModelFileImporter importer;
    CHECK(importer.Accepts(u8"glb"));
    Result<draconic::content::Instance*> imported =
        importer.Import(reinterpret_cast<const draconic::foundation::utf8char*>(DRACONIC_MI_TEST_GLB),
                        *project, *project->SourceDb().RootGroup(), nullptr, nullptr, nullptr);
    REQUIRE(imported.HasValue());
    draconic::content::Instance* manifestInst = imported.Value();
    REQUIRE(manifestInst != nullptr);
    CHECK(manifestInst->TypeName() == StringView(u8"ModelManifestAsset"));

    // The fan-out landed in a subgroup: meshes + a manifest at minimum.
    draconic::content::Group* modelGroup =
        project->SourceDb().RootGroup()->GetGroup(u8"character-oozi");
    REQUIRE(modelGroup != nullptr);
    CHECK(modelGroup->Instances().Size() >= 2u);

    RefPtr<ISerializable> object = manifestInst->ReadObject();
    auto* manifestAsset = Cast<modelimporter::ModelManifestAsset>(object.Get());
    REQUIRE(manifestAsset != nullptr);
    REQUIRE(manifestAsset->manifest.meshGuids.Size() >= 1u);
    CHECK(manifestAsset->manifest.nodes.Size() >= 1u);

    // Cook EVERYTHING through the incremental driver (the real pipeline path).
    BuilderRegistry builders;
    auto add = [&](auto* builder)
    { builders.Register(UniquePtr<IAssetBuilder>(builder, DefaultAllocator())); };
    add(DefaultAllocator().New<draconic::texture::TextureAssetBuilder>());
    add(DefaultAllocator().New<draconic::geometry::StaticMeshAssetBuilder>());
    add(DefaultAllocator().New<draconic::geometry::SkinnedMeshAssetBuilder>());
    add(DefaultAllocator().New<draconic::materials::MaterialAssetBuilder>());
    add(DefaultAllocator().New<draconic::animation::SkeletonAssetBuilder>());
    add(DefaultAllocator().New<draconic::animation::AnimationClipAssetBuilder>());
    add(DefaultAllocator().New<modelimporter::ModelManifestAssetBuilder>());

    draconic::vfs::NativeFileSystem sourcesFs(project->SourcesRoot().AsView());
    draconic::vfs::NativeFileSystem cacheFs(project->CacheRoot().AsView());
    CookDriver driver(project->SourceDb(), project->CookedDb(), builders, &sourcesFs, &cacheFs);

    CookPlan plan = driver.Plan();
    CHECK(plan.dirty.Size() >= 2u); // manifest + meshes (+ any materials/skeleton/clips)
    CookStats stats = driver.Execute(plan);
    CHECK(stats.failed == 0u);
    CHECK(stats.cooked == plan.dirty.Size());
    CHECK(driver.Plan().dirty.IsEmpty()); // incremental: everything clean now

    // Products landed under the source guids: the manifest product + a bindable mesh.
    CHECK(project->CookedDb().GetInstance(manifestInst->Id()) != nullptr);
    const Guid meshGuid = manifestAsset->manifest.meshGuids[0];
    RefPtr<ISerializable> meshProduct = project->CookedDb().ReadObject(meshGuid);
    REQUIRE(meshProduct.Get() != nullptr);

    cleanTree();
}

// Regression (user-reported): dropping a .gltf with EXTERNAL sidecars (Fox.bin, Texture.png)
// failed - the importer loaded from the Sources/ copy where the sidecars don't exist. It now
// loads from the original path and copies the referenced sidecars into Sources/.
TEST_CASE("model-import: external-sidecar .gltf imports and its sidecars land in Sources")
{
    using namespace draconic::editor;

    modelimporter::RegisterModelManifestAsset();
    draconic::texture::RegisterTextureAsset();
    draconic::geometry::RegisterMeshAssets();
    draconic::materials::RegisterMaterialAsset();
    draconic::animation::RegisterAnimationAssets();

    const StringView dir = u8"draconic_gltf_import_project";
    auto cleanTree = [&]()
    {
        for (StringView sub : {u8"Content", u8"Cooked", u8"Sources", u8".cache"})
        {
            draconic::vfs::NativeFileSystem fs(PathJoin(dir, sub).AsView());
            Array<draconic::vfs::DirEntry> tops;
            if (fs.AsEnumerable()->Enumerate(u8"", tops).IsOk())
            {
                for (const auto& top : tops)
                {
                    if (!top.isDirectory)
                    {
                        (void)fs.AsWritable()->Delete(top.name.AsView());
                        continue;
                    }
                    Array<draconic::vfs::DirEntry> inner;
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
    REQUIRE(EditorProject::Create(dir, u8"P").IsOk());
    UniquePtr<EditorProject> project = EditorProject::Open(dir);
    REQUIRE(static_cast<bool>(project));

    modelimporter::ModelFileImporter importer;
    Result<draconic::content::Instance*> imported =
        importer.Import(reinterpret_cast<const draconic::foundation::utf8char*>(DRACONIC_MI_TEST_FOX),
                        *project, *project->SourceDb().RootGroup(), nullptr, nullptr, nullptr);
    REQUIRE(imported.HasValue());
    REQUIRE(imported.Value() != nullptr);

    // Main file + both referenced sidecars are in Sources/.
    CHECK(FileExists(PathJoin(dir, u8"Sources/Fox.gltf").AsView()));
    CHECK(FileExists(PathJoin(dir, u8"Sources/Fox.bin").AsView()));
    CHECK(FileExists(PathJoin(dir, u8"Sources/Texture.png").AsView()));

    // The fan-out produced meshes + the Fox's animation clips.
    RefPtr<ISerializable> object = imported.Value()->ReadObject();
    auto* manifest = Cast<modelimporter::ModelManifestAsset>(object.Get());
    REQUIRE(manifest != nullptr);
    CHECK(manifest->manifest.meshGuids.Size() >= 1u);
    CHECK(manifest->manifest.animationGuids.Size() >= 1u); // Survey/Walk/Run

    cleanTree();
}

// Regression (user-reported): a freshly imported model's material rendered UNTEXTURED when
// picked directly (the Sandbox spawn-composite path wired textures; the self-contained
// MaterialSource path must too). Full chain: import -> cook -> bind material -> the albedo
// texture is installed as the material's default.
TEST_CASE("model-import: a bound material carries its albedo texture")
{
    using namespace draconic::editor;

    modelimporter::RegisterModelManifestAsset();
    draconic::model::RegisterModelResourceTypes();
    draconic::texture::RegisterTextureAsset();
    draconic::geometry::RegisterMeshAssets();
    draconic::materials::RegisterMaterialAsset();
    draconic::animation::RegisterAnimationAssets();

    const StringView dir = u8"draconic_mat_tex_project";
    auto cleanTree = [&]()
    {
        for (StringView sub : {u8"Content", u8"Cooked", u8"Sources", u8".cache"})
        {
            draconic::vfs::NativeFileSystem fs(PathJoin(dir, sub).AsView());
            Array<draconic::vfs::DirEntry> tops;
            if (fs.AsEnumerable()->Enumerate(u8"", tops).IsOk())
            {
                for (const auto& top : tops)
                {
                    if (!top.isDirectory)
                    {
                        (void)fs.AsWritable()->Delete(top.name.AsView());
                        continue;
                    }
                    Array<draconic::vfs::DirEntry> inner;
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
    REQUIRE(EditorProject::Create(dir, u8"P").IsOk());
    UniquePtr<EditorProject> project = EditorProject::Open(dir);
    REQUIRE(static_cast<bool>(project));

    // Import the Duck (textured, static) + cook everything.
    modelimporter::ModelFileImporter importer;
    Result<draconic::content::Instance*> imported =
        importer.Import(reinterpret_cast<const draconic::foundation::utf8char*>(DRACONIC_MI_TEST_DUCK),
                        *project, *project->SourceDb().RootGroup(), nullptr, nullptr, nullptr);
    REQUIRE(imported.HasValue());

    RefPtr<ISerializable> object = imported.Value()->ReadObject();
    auto* manifest = Cast<modelimporter::ModelManifestAsset>(object.Get());
    REQUIRE(manifest != nullptr);
    REQUIRE(manifest->manifest.materialGuids.Size() >= 1u);
    const Guid matGuid = manifest->manifest.materialGuids[0];

    BuilderRegistry builders;
    auto add = [&](auto* builder)
    { builders.Register(UniquePtr<IAssetBuilder>(builder, DefaultAllocator())); };
    add(DefaultAllocator().New<draconic::texture::TextureAssetBuilder>());
    add(DefaultAllocator().New<draconic::geometry::StaticMeshAssetBuilder>());
    add(DefaultAllocator().New<draconic::geometry::SkinnedMeshAssetBuilder>());
    add(DefaultAllocator().New<draconic::materials::MaterialAssetBuilder>());
    add(DefaultAllocator().New<draconic::animation::SkeletonAssetBuilder>());
    add(DefaultAllocator().New<draconic::animation::AnimationClipAssetBuilder>());
    add(DefaultAllocator().New<modelimporter::ModelManifestAssetBuilder>());

    draconic::vfs::NativeFileSystem sourcesFs(project->SourcesRoot().AsView());
    draconic::vfs::NativeFileSystem cacheFs(project->CacheRoot().AsView());
    CookDriver driver(project->SourceDb(), project->CookedDb(), builders, &sourcesFs, &cacheFs);
    CookPlan plan = driver.Plan();
    CookStats stats = driver.Execute(plan);
    REQUIRE(stats.failed == 0u);

    // Bind the material like the editor does (null GPU device backs the texture factory).
    // Device FIRST: it must outlive the manager's cached products (their destructors release
    // GPU objects through it).
    draconic::rhi::null::NullDevice device{DefaultAllocator()};
    resource::ResourceManager resources(project->CookedDb());
    draconic::geometry::StaticMeshFactory meshFactory;
    draconic::materials::MaterialFactory materialFactory;
    draconic::texture::TextureFactory textureFactory(device);
    resources.AddFactory(&meshFactory);
    resources.AddFactory(&materialFactory);
    resources.AddFactory(&textureFactory);

    resource::Proxy<draconic::materials::Material> material =
        resources.Bind<draconic::materials::Material>(matGuid);
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
    // glTF-convention (G = roughness, B = metalness; R = A = 255) - feeding either source
    // directly into the packed slot would bleed its value across BOTH channels.
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
        draconic::modelimporter::BakePackedMetallicRoughness(mdl, roughIdx, metalIdx, w, h);
    REQUIRE(packed.Size() == 2u * 2u * 4u);
    CHECK(w == 2u);
    CHECK(h == 2u);
    CHECK(packed[0] == 255u); // R unused
    CHECK(packed[1] == 200u); // G = roughness
    CHECK(packed[2] == 60u);  // B = metalness
    CHECK(packed[3] == 255u);

    // A missing map bakes identity (255) so the scalar factor carries the value.
    Array<u8> roughOnly =
        draconic::modelimporter::BakePackedMetallicRoughness(mdl, roughIdx, -1, w, h);
    REQUIRE(roughOnly.Size() == 2u * 2u * 4u);
    CHECK(roughOnly[1] == 200u);
    CHECK(roughOnly[2] == 255u);

    // Usage classification: both sources are data maps -> LINEAR.
    Array<bool> linear;
    auto* mat = new model::ModelMaterial();
    mat->separateRoughnessTextureIndex = roughIdx;
    mat->separateMetalnessTextureIndex = metalIdx;
    mdl.addMaterial(mat);
    draconic::modelimporter::ClassifyLinearTextures(mdl, linear);
    REQUIRE(linear.Size() == 2u);
    CHECK(linear[0]);
    CHECK(linear[1]);
}

TEST_CASE("mesh convert: missing tangent stream generates tangents (DamagedHelmet class)")
{
    // A quad in the XY plane, normal +Z, with U mapped along +Y - so the generated tangent
    // must be ~(0,1,0), NOT the {1,0,0} default a missing stream used to leave behind.
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
    modelimporter::StaticMeshSourceFromModel(mesh, source);
    REQUIRE(source.vertexBlob.Size() == 4 * sizeof(geometry::StaticMeshVertex));
    const auto* out = reinterpret_cast<const geometry::StaticMeshVertex*>(source.vertexBlob.Data());
    for (usize i = 0; i < 4; ++i)
    {
        const Float4 t = out[i].tangent;
        CHECK(Abs(t.y) == doctest::Approx(1.0f).epsilon(0.01)); // U runs along +Y
        CHECK(Abs(t.x) < 0.01f);
        CHECK(Abs(t.z) < 0.01f);
        // This U-along-Y mapping is MIRRORED: B = dP/dv = +X but cross(N,T) = -X, so the
        // generated handedness must be -1 (also proves w isn't stuck at the +1 default).
        CHECK(t.w == doctest::Approx(-1.0f));
        CHECK(Abs(Dot(Float3{t.x, t.y, t.z}, out[i].normal)) < 0.01f);
    }

    // An AUTHORED tangent stream passes through untouched (no regeneration).
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
    modelimporter::StaticMeshSourceFromModel(tmesh, tsource);
    const auto* tout =
        reinterpret_cast<const geometry::StaticMeshVertex*>(tsource.vertexBlob.Data());
    for (usize i = 0; i < 4; ++i)
    {
        CHECK(tout[i].tangent.z == doctest::Approx(1.0f));
        CHECK(tout[i].tangent.w == doctest::Approx(-1.0f));
    }
}

// Regression (user-reported): delete a model's group -> reimport the same file -> recook ->
// the editor crashed with garbage vkCreateImage params (a texture product misparsed). The
// suspicious seam: EnsureProduct adopts a same-named cooked instance from the PREVIOUS
// generation via CreateInstanceWithId's return-existing-by-name behavior, breaking the
// "product guid == source guid" invariant. This walks the exact user flow at DB level.
TEST_CASE("cook: delete group -> reimport -> recook keeps product identities clean")
{
    using namespace draconic::editor;

    modelimporter::RegisterModelManifestAsset();
    draconic::texture::RegisterTextureAsset();
    draconic::geometry::RegisterMeshAssets();
    draconic::materials::RegisterMaterialAsset();
    draconic::animation::RegisterAnimationAssets();
    // Product type registration (the test reads a texture product back).
    GlobalTypeRegistry().Register(draconic::texture::TextureResource::StaticType());
    RegisterSerializable<draconic::texture::TextureResource>();

    const StringView dir = u8"draconic_reimport_identity_project";
    auto cleanTree = [&]()
    {
        for (StringView sub : {u8"Content", u8"Cooked", u8"Sources", u8".cache"})
        {
            draconic::vfs::NativeFileSystem fs(PathJoin(dir, sub).AsView());
            Array<draconic::vfs::DirEntry> tops;
            if (fs.AsEnumerable()->Enumerate(u8"", tops).IsOk())
            {
                for (const auto& top : tops)
                {
                    if (!top.isDirectory)
                    {
                        (void)fs.AsWritable()->Delete(top.name.AsView());
                        continue;
                    }
                    Array<draconic::vfs::DirEntry> inner;
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
    REQUIRE(EditorProject::Create(dir, u8"P").IsOk());
    UniquePtr<EditorProject> project = EditorProject::Open(dir);
    REQUIRE(static_cast<bool>(project));

    BuilderRegistry builders;
    auto add = [&](auto* builder)
    { builders.Register(UniquePtr<IAssetBuilder>(builder, DefaultAllocator())); };
    add(DefaultAllocator().New<draconic::texture::TextureAssetBuilder>());
    add(DefaultAllocator().New<draconic::geometry::StaticMeshAssetBuilder>());
    add(DefaultAllocator().New<draconic::geometry::SkinnedMeshAssetBuilder>());
    add(DefaultAllocator().New<draconic::materials::MaterialAssetBuilder>());
    add(DefaultAllocator().New<draconic::animation::SkeletonAssetBuilder>());
    add(DefaultAllocator().New<draconic::animation::AnimationClipAssetBuilder>());
    add(DefaultAllocator().New<modelimporter::ModelManifestAssetBuilder>());

    draconic::vfs::NativeFileSystem sourcesFs(project->SourcesRoot().AsView());
    draconic::vfs::NativeFileSystem cacheFs(project->CacheRoot().AsView());
    CookDriver driver(project->SourceDb(), project->CookedDb(), builders, &sourcesFs, &cacheFs);

    // Duck has a texture (the crashing product kind). Import + cook generation 1.
    modelimporter::ModelFileImporter importer;
    Result<draconic::content::Instance*> firstImport =
        importer.Import(reinterpret_cast<const draconic::foundation::utf8char*>(DRACONIC_MI_TEST_DUCK),
                        *project, *project->SourceDb().RootGroup(), nullptr, nullptr, nullptr);
    REQUIRE(firstImport.HasValue());

    draconic::content::Group* duckGroup = project->SourceDb().RootGroup()->GetGroup(u8"Duck");
    REQUIRE(duckGroup != nullptr);
    Array<Guid> oldGuids;
    for (draconic::content::Instance* inst : duckGroup->Instances())
    {
        oldGuids.PushBack(inst->Id());
    }
    const usize assetCount = oldGuids.Size();
    REQUIRE(assetCount >= 3u); // manifest + mesh + material + texture

    CookPlan plan1 = driver.Plan();
    CookStats stats1 = driver.Execute(plan1);
    CHECK(stats1.failed == 0u);
    for (const Guid& id : oldGuids)
    {
        CHECK(project->CookedDb().GetInstance(id) != nullptr);
    }

    // === user step 1: delete the group (source side only - DeleteGroupNow's behavior) ===
    REQUIRE(project->SourceDb().DeleteGroup(*duckGroup).IsOk());

    // === user step 2: reimport the same file ===
    Result<draconic::content::Instance*> secondImport =
        importer.Import(reinterpret_cast<const draconic::foundation::utf8char*>(DRACONIC_MI_TEST_DUCK),
                        *project, *project->SourceDb().RootGroup(), nullptr, nullptr, nullptr);
    REQUIRE(secondImport.HasValue());
    draconic::content::Group* duckGroup2 = project->SourceDb().RootGroup()->GetGroup(u8"Duck");
    REQUIRE(duckGroup2 != nullptr);
    Array<Guid> newGuids;
    for (draconic::content::Instance* inst : duckGroup2->Instances())
    {
        newGuids.PushBack(inst->Id());
    }
    REQUIRE(newGuids.Size() == assetCount);

    // === user step 3: the next cook (sweeps generation-1 orphans, cooks generation 2) ===
    CookPlan plan2 = driver.Plan();
    CHECK(plan2.orphans.Size() == assetCount); // EVERY dead source must be an orphan
    CookStats stats2 = driver.Execute(plan2);
    CHECK(stats2.failed == 0u);

    // Identity invariant: every generation-2 product exists UNDER ITS SOURCE GUID and
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
        draconic::content::Instance* product = project->CookedDb().GetInstance(id);
        REQUIRE(product != nullptr);
        draconic::content::Instance* source = project->SourceDb().GetInstance(id);
        REQUIRE(source != nullptr);
        CHECK(product->Name() == source->Name());
    }
    for (const Guid& id : oldGuids)
    {
        CHECK(project->CookedDb().GetInstance(id) == nullptr);
    }

    // Identity-guard regression (the guid-replay crash): plant a STALE same-named product
    // with a foreign guid and a CROSS-TYPED product under a real source guid, then force a
    // recook - EnsureProduct must remove both and land every product on source guid + type.
    {
        draconic::content::Group* cookedDuck = project->CookedDb().RootGroup()->GetGroup(u8"Duck");
        REQUIRE(cookedDuck != nullptr);
        // (a) name collision: same name as a real product, different guid.
        draconic::content::Instance* real = nullptr;
        for (const Guid& id : newGuids)
        {
            if (draconic::content::Instance* p = project->CookedDb().GetInstance(id))
            {
                real = p;
                break;
            }
        }
        REQUIRE(real != nullptr);
        const String collidedName(real->Name());
        const Guid foreign = Guid{0xDEADBEEFull, 0xFEEDF00Dull};
        (void)project->CookedDb().DeleteInstance(real->Id());
        draconic::content::Instance* stale = cookedDuck->CreateInstanceWithId(
            foreign, collidedName.AsView(), draconic::texture::TextureResource::StaticType());
        REQUIRE(stale != nullptr);
        CHECK(stale->Id() == foreign);

        CookPlan replan = driver.Plan(true); // force: every asset re-cooks
        CookStats restats = driver.Execute(replan);
        CHECK(restats.failed == 0u);
        for (const Guid& id : newGuids)
        {
            draconic::content::Instance* product = project->CookedDb().GetInstance(id);
            REQUIRE(product != nullptr);
            draconic::content::Instance* source = project->SourceDb().GetInstance(id);
            REQUIRE(source != nullptr);
            CHECK(product->Name() == source->Name());
        }
        CHECK(project->CookedDb().GetInstance(foreign) == nullptr); // stale removed
    }

    // Scoped plans: PlanFor(roots) covers the roots + their dependency closure ONLY.
    {
        Guid matGuid, meshGuid;
        for (draconic::content::Instance* inst : duckGroup2->Instances())
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

        // Force re-cooks the ROOT only; its clean dependency closure (textures) stays out.
        Guid matRoots[] = {matGuid};
        CookPlan forced = driver.PlanFor(Span<const Guid>{matRoots, 1}, true);
        REQUIRE(forced.dirty.Size() == 1u);
        CHECK(forced.dirty[0].source == matGuid);
        CookStats scopedStats = driver.Execute(forced);
        CHECK(scopedStats.failed == 0u);
        CHECK(scopedStats.cooked == 1u);
    }

    // The texture product must deserialize to a SANE TextureResource (the crash showed
    // garbage width/height/mips from a misparsed product).
    bool textureChecked = false;
    for (const Guid& id : newGuids)
    {
        draconic::content::Instance* product = project->CookedDb().GetInstance(id);
        if (product == nullptr || product->TypeName() != StringView(u8"TextureResource"))
        {
            continue;
        }
        RefPtr<ISerializable> object = project->CookedDb().ReadObject(id);
        auto* tex = Cast<draconic::texture::TextureResource>(object.Get());
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
    CHECK(modelimporter::ImportedTextureName(named, 0).AsView() == StringView(u8"BaseColor"));

    model::ModelTexture fromUri;
    fromUri.setUri(u8"textures/Default_albedo.jpg");
    CHECK(modelimporter::ImportedTextureName(fromUri, 3).AsView() ==
          StringView(u8"Default_albedo"));

    model::ModelTexture bare; // embedded, no identity -> indexed fallback
    CHECK(modelimporter::ImportedTextureName(bare, 7).AsView() == StringView(u8"tex.7"));
}

TEST_CASE("import: material sampler modes map from the source texture's sampler")
{
    CHECK(modelimporter::AddressModeFromWrap(model::TextureWrap::Repeat) == 0);
    CHECK(modelimporter::AddressModeFromWrap(model::TextureWrap::MirroredRepeat) == 1);
    CHECK(modelimporter::AddressModeFromWrap(model::TextureWrap::ClampToEdge) == 2);
}

TEST_CASE("import: sub-assets keep authored names (sanitized), indexed fallback otherwise")
{
    CHECK(modelimporter::ImportedAssetName(u8"Material_MR", u8"mat", 0).AsView() ==
          StringView(u8"Material_MR"));
    CHECK(modelimporter::ImportedAssetName(u8"mesh_helmet_LP", u8"mesh", 3).AsView() ==
          StringView(u8"mesh_helmet_LP"));
    // Path-hostile characters sanitize (names become envelope file names).
    CHECK(modelimporter::ImportedAssetName(u8"body/armor:v2", u8"mesh", 0).AsView() ==
          StringView(u8"body_armor_v2"));
    // No authored name -> indexed fallback.
    CHECK(modelimporter::ImportedAssetName(u8"", u8"anim", 4).AsView() == StringView(u8"anim.4"));
}

namespace
{
    // Recursive best-effort cleanup so EditorProject::Create starts from a clean slate
    // across test runs (same shape as the first test's cleanTree lambda, two levels deep).
    void CleanProjectTree(StringView dir)
    {
        for (StringView sub : {u8"Content", u8"Cooked", u8"Sources", u8".cache"})
        {
            draconic::vfs::NativeFileSystem fs(draconic::foundation::PathJoin(dir, sub).AsView());
            draconic::foundation::Array<draconic::vfs::DirEntry> tops;
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
                draconic::foundation::Array<draconic::vfs::DirEntry> inner;
                if (fs.AsEnumerable()->Enumerate(top.name.AsView(), inner).IsOk())
                {
                    for (const auto& e : inner)
                    {
                        draconic::foundation::String path =
                            draconic::foundation::PathJoin(top.name.AsView(), e.name.AsView());
                        (void)fs.AsWritable()->Delete(path.AsView());
                    }
                }
                (void)fs.AsWritable()->Delete(top.name.AsView());
            }
            (void)draconic::foundation::RemoveDirectory(draconic::foundation::PathJoin(dir, sub).AsView());
        }
        draconic::vfs::NativeFileSystem fs(dir);
        draconic::foundation::Array<draconic::vfs::DirEntry> entries;
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
        (void)draconic::foundation::RemoveDirectory(dir);
    }
}

TEST_CASE("model-import: options gate textures/materials/animations")
{
    using namespace draconic::editor;
    modelimporter::RegisterModelManifestAsset();
    draconic::texture::RegisterTextureAsset();
    draconic::geometry::RegisterMeshAssets();
    draconic::materials::RegisterMaterialAsset();
    draconic::animation::RegisterAnimationAssets();

    const StringView dir = u8"draconic_model_import_options_project";
    CleanProjectTree(dir);
    REQUIRE(EditorProject::Create(dir, u8"P").IsOk());
    UniquePtr<EditorProject> project = EditorProject::Open(dir);
    REQUIRE(static_cast<bool>(project));

    modelimporter::ModelFileImporter importer;

    // The importer advertises options, and their defaults import everything.
    RefPtr<ImportOptions> base = importer.CreateOptions();
    REQUIRE(base.Get() != nullptr);
    auto* options = static_cast<modelimporter::ModelImportOptions*>(base.Get());
    CHECK(options->importTextures);
    CHECK(options->importMaterials);
    CHECK(options->importAnimations);
    CHECK(options->generatePrefab);
    CHECK_FALSE(options->generateCollision); // opt-in
    CHECK_FALSE(options->collisionConvex);
    CHECK(options->Toggles().Size() == 6u);

    // Geometry-only import: no textures, no materials, no skeleton/clips in the fan-out.
    options->importTextures = false;
    options->importMaterials = false;
    options->importAnimations = false;
    Result<draconic::content::Instance*> imported =
        importer.Import(reinterpret_cast<const draconic::foundation::utf8char*>(DRACONIC_MI_TEST_GLB),
                        *project, *project->SourceDb().RootGroup(), options, nullptr, nullptr);
    REQUIRE(imported.HasValue());

    RefPtr<ISerializable> object = imported.Value()->ReadObject();
    auto* manifest = Cast<modelimporter::ModelManifestAsset>(object.Get());
    REQUIRE(manifest != nullptr);
    CHECK(manifest->manifest.meshGuids.Size() >= 1u); // geometry always imports
    CHECK(manifest->manifest.materialGuids.IsEmpty());
    CHECK(manifest->manifest.skeletonGuid.IsNil());
    CHECK(manifest->manifest.animationGuids.IsEmpty());

    draconic::content::Group* modelGroup =
        project->SourceDb().RootGroup()->GetGroup(u8"character-oozi");
    REQUIRE(modelGroup != nullptr);
    for (draconic::content::Instance* inst : modelGroup->Instances())
    {
        CHECK(inst->TypeName() != StringView(u8"TextureAsset"));
        CHECK(inst->TypeName() != StringView(u8"MaterialAsset"));
        CHECK(inst->TypeName() != StringView(u8"SkeletonAsset"));
        CHECK(inst->TypeName() != StringView(u8"AnimationClipAsset"));
    }
}

TEST_CASE("model-import: generate-collision emits CollisionShapeAssets wired to the meshes")
{
    using namespace draconic::editor;
    modelimporter::RegisterModelManifestAsset();
    draconic::texture::RegisterTextureAsset();
    draconic::geometry::RegisterMeshAssets();
    draconic::materials::RegisterMaterialAsset();
    draconic::animation::RegisterAnimationAssets();
    draconic::physics::RegisterPhysicsAssets();

    const StringView dir = u8"draconic_model_import_collision_project";
    CleanProjectTree(dir);
    REQUIRE(EditorProject::Create(dir, u8"P").IsOk());
    UniquePtr<EditorProject> project = EditorProject::Open(dir);
    REQUIRE(static_cast<bool>(project));

    modelimporter::ModelFileImporter importer;
    RefPtr<ImportOptions> base = importer.CreateOptions();
    auto* options = static_cast<modelimporter::ModelImportOptions*>(base.Get());
    options->generateCollision = true;
    options->collisionConvex = true;
    Result<draconic::content::Instance*> imported =
        importer.Import(reinterpret_cast<const draconic::foundation::utf8char*>(DRACONIC_MI_TEST_GLB),
                        *project, *project->SourceDb().RootGroup(), options, nullptr, nullptr);
    REQUIRE(imported.HasValue());

    RefPtr<ISerializable> object = imported.Value()->ReadObject();
    auto* manifest = Cast<modelimporter::ModelManifestAsset>(object.Get());
    REQUIRE(manifest != nullptr);
    // collisionGuids parallels meshGuids; STATIC meshes get shapes (skinned stay nil).
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
        draconic::content::Instance* inst = project->SourceDb().GetInstance(g);
        REQUIRE(inst != nullptr);
        RefPtr<ISerializable> shapeObject = inst->ReadObject();
        auto* shape = Cast<draconic::physics::CollisionShapeAsset>(shapeObject.Get());
        REQUIRE(shape != nullptr);
        CHECK(shape->sourceMesh == manifest->manifest.meshGuids[i]);
        CHECK(shape->cook == draconic::physics::CollisionCookKind::ConvexHull);
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
    using namespace draconic::editor;
    modelimporter::RegisterModelManifestAsset();
    draconic::texture::RegisterTextureAsset();
    draconic::geometry::RegisterMeshAssets();
    draconic::materials::RegisterMaterialAsset();
    draconic::animation::RegisterAnimationAssets();

    const StringView dir = u8"draconic_model_reimport_project";
    CleanProjectTree(dir);
    REQUIRE(EditorProject::Create(dir, u8"P").IsOk());
    UniquePtr<EditorProject> project = EditorProject::Open(dir);
    REQUIRE(static_cast<bool>(project));

    modelimporter::ModelFileImporter importer;
    Result<draconic::content::Instance*> first =
        importer.Import(reinterpret_cast<const draconic::foundation::utf8char*>(DRACONIC_MI_TEST_DUCK),
                        *project, *project->SourceDb().RootGroup(), nullptr, nullptr, nullptr);
    REQUIRE(first.HasValue());

    draconic::content::Group* duck = project->SourceDb().RootGroup()->GetGroup(u8"Duck");
    REQUIRE(duck != nullptr);
    HashMap<String, Guid> before;
    for (draconic::content::Instance* inst : duck->Instances())
    {
        before.InsertOrAssign(String(inst->Name()), inst->Id());
    }
    const usize assetCount = before.Size();
    REQUIRE(assetCount >= 3u);

    // Re-drop the SAME file with the group intact: every instance is REUSED by (name, type) -
    // guids survive (placed refs + the prefab keep working) and nothing duplicates as ".2".
    Result<draconic::content::Instance*> second =
        importer.Import(reinterpret_cast<const draconic::foundation::utf8char*>(DRACONIC_MI_TEST_DUCK),
                        *project, *project->SourceDb().RootGroup(), nullptr, nullptr, nullptr);
    REQUIRE(second.HasValue());
    CHECK(second.Value()->Id() == first.Value()->Id());

    draconic::content::Group* duckAfter = project->SourceDb().RootGroup()->GetGroup(u8"Duck");
    REQUIRE(duckAfter != nullptr);
    CHECK(duckAfter->Instances().Size() == assetCount); // no duplicates
    for (draconic::content::Instance* inst : duckAfter->Instances())
    {
        const Guid* old = before.Find(String(inst->Name()));
        REQUIRE(old != nullptr);   // same name set as the first import
        CHECK(*old == inst->Id()); // same guid: reuse, not re-mint
    }
}
