// Model->prefab generation (prefabs P3): a hand-authored model manifest becomes a spawnable
// PrefabDocument whose hierarchy + mesh/material/animation refs mirror the manifest, and a
// second generation REUSES the prefab instance (same guid - re-import propagates to placed
// instances through the standard rebuild machinery).

#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

import draconic.foundation;
import draconic.vfs;
import draconic.content;
import draconic.scene;
import draconic.scene.resource;
import draconic.engine.render;
import draconic.engine.animation;
import draconic.modelimporter;
import draconic.editor.scene;

using namespace draconic::foundation;
namespace scene = draconic::scene;
namespace render = draconic::render;
namespace animation = draconic::animation;
namespace modelimporter = draconic::modelimporter;

namespace
{
    void RemoveTreeMP(StringView root)
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
                    continue;
                }
                // One level of group subdirectories (the model group) - a stale "Prefab"
                // instance in there flips the regeneration check on reruns.
                Array<draconic::vfs::DirEntry> inner;
                if (fs.AsEnumerable()->Enumerate(e.name.AsView(), inner).IsOk())
                {
                    for (const auto& f : inner)
                    {
                        String path = PathJoin(e.name.AsView(), f.name.AsView());
                        (void)fs.AsWritable()->Delete(path.AsView());
                    }
                }
                (void)fs.AsWritable()->Delete(e.name.AsView());
            }
        }
        (void)RemoveDirectory(root);
    }
}

TEST_CASE("model-prefab: manifest -> spawnable prefab; regeneration reuses the instance")
{
    modelimporter::RegisterModelManifestAsset();
    GlobalTypeRegistry().Register(scene::PrefabDocument::StaticType());
    RegisterSerializable<scene::PrefabDocument>();

    const StringView dir = u8"draconic_model_prefab_test_db";
    RemoveTreeMP(dir);
    (void)CreateDirectory(dir);
    draconic::vfs::NativeFileSystem mount(dir);
    draconic::content::ContentDatabase db(mount, BinarySerializerFactory(), u8".rasset");

    // Hand-authored manifest: root node + a multi-material static mesh node + a skinned node.
    const Guid meshStatic{0x51, 0x1};
    const Guid meshSkinned{0x52, 0x2};
    const Guid matA{0x61, 0x1};
    const Guid matB{0x62, 0x2};
    const Guid skeleton{0x71, 0x1};
    const Guid clip{0x72, 0x1};

    modelimporter::ModelManifestAsset asset;
    asset.manifest.meshGuids.PushBack(meshStatic);
    asset.manifest.meshGuids.PushBack(meshSkinned);
    asset.manifest.meshSkinned.PushBack(0);
    asset.manifest.meshSkinned.PushBack(1);
    asset.manifest.meshMaterial.PushBack(1); // static mesh's first part uses material B
    asset.manifest.meshMaterial.PushBack(0);
    asset.manifest.materialGuids.PushBack(matA);
    asset.manifest.materialGuids.PushBack(matB);
    asset.manifest.skeletonGuid = skeleton;
    asset.manifest.animationGuids.PushBack(clip);
    {
        draconic::model::ModelNode rootNode;
        rootNode.name = String(u8"Armature");
        rootNode.parentIndex = -1;
        asset.manifest.nodes.PushBack(Move(rootNode));
        draconic::model::ModelNode meshNode;
        meshNode.name = String(u8"Body");
        meshNode.parentIndex = 0;
        meshNode.meshIndex = 0;
        meshNode.localTransform.position = Float3{1.0f, 2.0f, 3.0f};
        asset.manifest.nodes.PushBack(Move(meshNode));
        draconic::model::ModelNode skinNode;
        skinNode.name = String(u8"Skin");
        skinNode.parentIndex = 0;
        skinNode.meshIndex = 1;
        asset.manifest.nodes.PushBack(Move(skinNode));
    }

    draconic::content::Group* group = db.RootGroup()->CreateGroup(u8"Fox");
    REQUIRE(group != nullptr);
    draconic::content::Instance* manifestInst =
        group->CreateInstance(u8"Fox", modelimporter::ModelManifestAsset::StaticType());
    REQUIRE(manifestInst != nullptr);
    REQUIRE(manifestInst->WriteObject(asset).IsOk());

    draconic::editor::ModelPrefabResult generated =
        draconic::editor::GenerateModelPrefab(*manifestInst);
    REQUIRE(generated.instance != nullptr);
    CHECK(!generated.regenerated);
    CHECK(generated.instance->Name() == StringView(u8"Prefab"));
    CHECK(generated.instance->TypeName() == StringView(u8"PrefabDocument"));
    const Guid prefabId = generated.instance->Id();

    // Spawn the payload: hierarchy + refs mirror the manifest.
    UniquePtr<IStream> payload = generated.instance->ReadData(u8"scene");
    REQUIRE(payload.Get() != nullptr);
    scene::Scene level(u8"level");
    auto* meshes = level.AddSystem<render::MeshComponentManager>();
    auto* anims = level.AddSystem<animation::SkeletalAnimationComponentManager>();
    scene::EntityHandle root = scene::SpawnPrefab(level, *payload, prefabId);
    REQUIRE(root.IsAssigned());
    CHECK(level.GetEntityName(root) == StringView(u8"Fox"));

    scene::EntityHandle armature = level.GetFirstChild(root);
    REQUIRE(armature.IsAssigned());
    CHECK(level.GetEntityName(armature) == StringView(u8"Armature"));

    usize meshCount = 0;
    bool sawStatic = false, sawSkinned = false;
    meshes->ForEach(
        [&](render::MeshComponent& c, scene::EntityHandle e)
        {
            ++meshCount;
            if (c.mesh.id == meshStatic)
            {
                sawStatic = true;
                REQUIRE(c.materials.Size() == 2u); // the unified material list
                CHECK(c.materials[0].id == matA);
                CHECK(c.materials[1].id == matB);
                const Transform t = level.GetLocalTransform(e);
                CHECK(t.position.x == 1.0f);
            }
            if (c.mesh.id == meshSkinned)
            {
                sawSkinned = true;
                REQUIRE(c.materials.Size() == 2u);
            }
        });
    CHECK(meshCount == 2u);
    CHECK(sawStatic);
    CHECK(sawSkinned);

    usize animCount = 0;
    anims->ForEach(
        [&](animation::SkeletalAnimationComponent& c, scene::EntityHandle)
        {
            ++animCount;
            CHECK(c.skeleton.id == skeleton);
            CHECK(c.clip.id == clip);
        });
    CHECK(animCount == 1u); // only the SKINNED node animates

    // Regeneration finds + reuses the instance: same guid, refreshed payload.
    draconic::editor::ModelPrefabResult again =
        draconic::editor::GenerateModelPrefab(*manifestInst);
    REQUIRE(again.instance != nullptr);
    CHECK(again.regenerated);
    CHECK(again.instance->Id() == prefabId);

    RemoveTreeMP(dir);
}
