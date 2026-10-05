// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Model->prefab generation: a hand-authored model manifest becomes a spawnable
// PrefabDocument whose hierarchy + mesh/material/animation refs mirror the manifest, and a
// second generation REUSES the prefab instance (same guid - re-import propagates to placed
// instances through the standard rebuild machinery).

#include <doctest/doctest.h>
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

#include <cmath>

import foundation.core;
import foundation.vfs;
import foundation.content;
import foundation.scene;
import foundation.scene.resource;
import engine.render;
import engine.animation;
import modelimporter;
import pipeline.importer;
import scene.pipeline;

using namespace foundation::core;
namespace scene = foundation::scene;

namespace
{
    void RemoveTreeMP(StringView root)
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
                    continue;
                }
                // One level of group subdirectories (the model group) - a stale "Prefab"
                // instance in there flips the regeneration check on reruns.
                Array<foundation::vfs::DirEntry> inner;
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
    pipeline::RegisterModelManifestAsset();
    // Component reflection carries the DataVersion the versioned payloads gate on (MeshComponent v3
    // = unified materials array); register it so the round-trip is order-independent.
    engine::render::RegisterRenderComponentReflection();
    engine::animation::RegisterAnimationComponentReflection();
    GlobalTypeRegistry().Register(scene::PrefabDocument::StaticType());
    RegisterSerializable<scene::PrefabDocument>();

    const StringView dir = u8"scratch_model_prefab_test_db";
    RemoveTreeMP(dir);
    (void)CreateDirectory(dir);
    foundation::vfs::NativeFileSystem mount(dir, foundation::core::DefaultAllocator());
    foundation::content::ContentDatabase db(foundation::core::DefaultAllocator(), mount, BinarySerializerFactory(), u8".rasset");

    // Hand-authored manifest: root node + a multi-material static mesh node + a skinned node.
    const Guid meshStatic{0x51, 0x1};
    const Guid meshSkinned{0x52, 0x2};
    const Guid matA{0x61, 0x1};
    const Guid matB{0x62, 0x2};
    const Guid skeleton{0x71, 0x1};
    const Guid clip{0x72, 0x1};

    pipeline::ModelManifestAsset asset;
    asset.manifest.meshGuids.PushBack(meshStatic);
    asset.manifest.meshGuids.PushBack(meshSkinned);
    asset.manifest.meshSkinned.PushBack(0);
    asset.manifest.meshSkinned.PushBack(1);
    asset.manifest.meshMaterial.PushBack(1); // static mesh's first part uses material B
    asset.manifest.meshMaterial.PushBack(0);
    asset.manifest.materialGuids.PushBack(matA);
    asset.manifest.materialGuids.PushBack(matB);
    {
        // v2 slots: the static mesh draws material B only (its submeshes index slot 0 = B);
        // the skinned mesh has NO slots entry (a v1-shaped mesh) and keeps the whole list.
        foundation::model::ModelMeshMaterialSlots staticSlots;
        staticSlots.slots.PushBack(1);
        asset.manifest.meshMaterialSlots.PushBack(Move(staticSlots));
        asset.manifest.meshMaterialSlots.PushBack(foundation::model::ModelMeshMaterialSlots{});
    }
    asset.manifest.skeletonGuid = skeleton;
    asset.manifest.animationGuids.PushBack(clip);
    {
        foundation::model::ModelNode rootNode;
        rootNode.name = String(u8"Armature");
        rootNode.parentIndex = -1;
        asset.manifest.nodes.PushBack(Move(rootNode));
        foundation::model::ModelNode meshNode;
        meshNode.name = String(u8"Body");
        meshNode.parentIndex = 0;
        meshNode.meshIndex = 0;
        meshNode.localTransform.position = Float3{1.0f, 2.0f, 3.0f};
        asset.manifest.nodes.PushBack(Move(meshNode));
        foundation::model::ModelNode skinNode;
        skinNode.name = String(u8"Skin");
        skinNode.parentIndex = 0;
        skinNode.meshIndex = 1;
        asset.manifest.nodes.PushBack(Move(skinNode));
    }

    foundation::content::Group* group = db.RootGroup()->CreateGroup(u8"Fox");
    REQUIRE(group != nullptr);
    foundation::content::Instance* manifestInst =
        group->CreateInstance(u8"Fox", pipeline::ModelManifestAsset::StaticType());
    REQUIRE(manifestInst != nullptr);
    REQUIRE(manifestInst->WriteObject(asset).IsOk());

    pipeline::ModelPrefabResult generated =
        pipeline::GenerateModelPrefab(DefaultAllocator(), *manifestInst);
    REQUIRE(generated.instance != nullptr);
    CHECK(!generated.regenerated);
    CHECK(generated.instance->Name() == StringView(u8"Prefab"));
    CHECK(generated.instance->TypeName() == StringView(u8"PrefabDocument"));
    const Guid prefabId = generated.instance->Id();

    // Spawn the payload: hierarchy + refs mirror the manifest.
    UniquePtr<IStream> payload = generated.instance->ReadData(u8"scene");
    REQUIRE(payload.Get() != nullptr);
    scene::Scene level(DefaultAllocator(), u8"level");
    auto* meshes = level.AddSystem<engine::render::MeshComponentManager>();
    auto* anims = level.AddSystem<engine::animation::SkeletalAnimationComponentManager>();
    scene::EntityHandle root = scene::SpawnPrefab(level, *payload, prefabId);
    REQUIRE(root.IsAssigned());
    CHECK(level.GetEntityName(root) == StringView(u8"Fox"));

    scene::EntityHandle armature = level.GetFirstChild(root);
    REQUIRE(armature.IsAssigned());
    CHECK(level.GetEntityName(armature) == StringView(u8"Armature"));

    usize meshCount = 0;
    bool sawStatic = false, sawSkinned = false;
    meshes->ForEach(
        [&](engine::render::MeshComponent& c, scene::EntityHandle e)
        {
            ++meshCount;
            if (c.mesh.id == meshStatic)
            {
                sawStatic = true;
                REQUIRE(c.materials.Size() == 1u); // only the slot the mesh draws
                CHECK(c.materials[0].id == matB);
                const Transform t = level.GetLocalTransform(e);
                CHECK(t.position.x == 1.0f);
            }
            if (c.mesh.id == meshSkinned)
            {
                sawSkinned = true;
                REQUIRE(c.materials.Size() == 2u); // no slots recorded: the whole list
                CHECK(c.materials[0].id == matA);
                CHECK(c.materials[1].id == matB);
            }
        });
    CHECK(meshCount == 2u);
    CHECK(sawStatic);
    CHECK(sawSkinned);

    usize animCount = 0;
    anims->ForEach(
        [&](engine::animation::SkeletalAnimationComponent& c, scene::EntityHandle e)
        {
            ++animCount;
            CHECK(c.skeleton.id == skeleton);
            CHECK(c.clip.id == clip);
            // ONE animator on the prefab ROOT feeds the skinned nodes through
            // prefab-remapped EntityRefs - never one animator per part.
            CHECK(e == root);
            REQUIRE(c.meshEntities.Size() == 1u);
            const scene::EntityHandle fed = level.FindEntity(c.meshEntities[0].id);
            REQUIRE(fed.IsAssigned());
            engine::render::MeshComponent* fedMesh = meshes->Get(fed);
            REQUIRE(fedMesh != nullptr);
            CHECK(fedMesh->mesh.id == meshSkinned);
        });
    CHECK(animCount == 1u); // one animator for the whole model

    // Regeneration finds + reuses the instance: same guid, refreshed payload.
    pipeline::ModelPrefabResult again =
        pipeline::GenerateModelPrefab(DefaultAllocator(), *manifestInst);
    REQUIRE(again.instance != nullptr);
    CHECK(again.regenerated);
    CHECK(again.instance->Id() == prefabId);

    RemoveTreeMP(dir);
}

TEST_CASE("model-scene: manifest -> standalone scene; regeneration reuses the instance")
{
    pipeline::RegisterModelManifestAsset();
    // Component reflection carries the DataVersion the versioned payloads gate on (MeshComponent v3
    // = unified materials array); register it so the materials round-trip is order-independent.
    engine::render::RegisterRenderComponentReflection();
    engine::animation::RegisterAnimationComponentReflection();
    GlobalTypeRegistry().Register(scene::SceneDocument::StaticType());
    RegisterSerializable<scene::SceneDocument>();

    const StringView dir = u8"scratch_model_scene_test_db";
    RemoveTreeMP(dir);
    (void)CreateDirectory(dir);
    foundation::vfs::NativeFileSystem mount(dir, foundation::core::DefaultAllocator());
    foundation::content::ContentDatabase db(foundation::core::DefaultAllocator(), mount, BinarySerializerFactory(), u8".rasset");

    // Root node + a single static-mesh child node (Crate -> Root -> Body).
    const Guid meshStatic{0x51, 0x1};
    const Guid matA{0x61, 0x1};
    pipeline::ModelManifestAsset asset;
    asset.manifest.meshGuids.PushBack(meshStatic);
    asset.manifest.meshSkinned.PushBack(0);
    asset.manifest.materialGuids.PushBack(matA);
    {
        foundation::model::ModelNode rootNode;
        rootNode.name = String(u8"Root");
        rootNode.parentIndex = -1;
        asset.manifest.nodes.PushBack(Move(rootNode));
        foundation::model::ModelNode meshNode;
        meshNode.name = String(u8"Body");
        meshNode.parentIndex = 0;
        meshNode.meshIndex = 0;
        meshNode.localTransform.position = Float3{4.0f, 5.0f, 6.0f};
        asset.manifest.nodes.PushBack(Move(meshNode));
    }

    foundation::content::Group* group = db.RootGroup()->CreateGroup(u8"Crate");
    REQUIRE(group != nullptr);
    foundation::content::Instance* manifestInst =
        group->CreateInstance(u8"Crate", pipeline::ModelManifestAsset::StaticType());
    REQUIRE(manifestInst != nullptr);
    REQUIRE(manifestInst->WriteObject(asset).IsOk());

    pipeline::ModelPrefabResult generated = pipeline::GenerateModelScene(DefaultAllocator(), *manifestInst);
    REQUIRE(generated.instance != nullptr);
    CHECK(!generated.regenerated);
    CHECK(generated.instance->Name() == StringView(u8"Scene"));
    CHECK(generated.instance->TypeName() == StringView(u8"SceneDocument"));
    const Guid sceneId = generated.instance->Id();

    // The SceneDocument primary carries the name for discovery.
    RefPtr<ISerializable> doc = generated.instance->ReadObject();
    scene::SceneDocument* sd = Cast<scene::SceneDocument>(doc.Get());
    REQUIRE(sd != nullptr);
    CHECK(sd->name == StringView(u8"Crate"));

    // Load it back into a fresh scene whose managers are injected first (as a subsystem would):
    // the node hierarchy + mesh refs round-trip, a bare scene with NO default camera/light.
    scene::Scene loaded{DefaultAllocator()};
    auto* meshes = loaded.AddSystem<engine::render::MeshComponentManager>();
    loaded.AddSystem<engine::animation::SkeletalAnimationComponentManager>();
    REQUIRE(scene::LoadScene(*generated.instance, loaded).IsOk());
    CHECK(loaded.EntityCount() == 3u); // Crate root + Root + Body, nothing auto-added

    scene::EntityHandle sceneRoot = loaded.FindEntityByName(u8"Crate");
    REQUIRE(sceneRoot.IsAssigned());
    scene::EntityHandle body = loaded.FindEntityByName(u8"Body");
    REQUIRE(body.IsAssigned());
    const Transform bodyT = loaded.GetLocalTransform(body);
    CHECK(bodyT.position.x == 4.0f);

    usize meshCount = 0;
    meshes->ForEach(
        [&](engine::render::MeshComponent& c, scene::EntityHandle)
        {
            ++meshCount;
            CHECK(c.mesh.id == meshStatic);
            REQUIRE(c.materials.Size() == 1u);
            CHECK(c.materials[0].id == matA);
        });
    CHECK(meshCount == 1u);

    // Regeneration finds + reuses the instance: same guid, refreshed payload.
    pipeline::ModelPrefabResult regen = pipeline::GenerateModelScene(DefaultAllocator(), *manifestInst);
    REQUIRE(regen.instance != nullptr);
    CHECK(regen.regenerated);
    CHECK(regen.instance->Id() == sceneId);

    RemoveTreeMP(dir);
}

// Sedulous 6b33743b: what a finished import generates is the pipeline's decision, the same in
// every host: the prefab by default, the scene when asked, nothing for what is not a model.
TEST_CASE("model-prefab: an import generates by its options, and only for a model")
{
    pipeline::RegisterModelManifestAsset();
    engine::render::RegisterRenderComponentReflection();
    engine::animation::RegisterAnimationComponentReflection();
    GlobalTypeRegistry().Register(scene::PrefabDocument::StaticType());
    RegisterSerializable<scene::PrefabDocument>();
    GlobalTypeRegistry().Register(scene::SceneDocument::StaticType());
    RegisterSerializable<scene::SceneDocument>();

    const StringView dir = u8"scratch_model_import_generation_db";
    RemoveTreeMP(dir);
    (void)CreateDirectory(dir);
    foundation::vfs::NativeFileSystem mount(dir, DefaultAllocator());
    foundation::content::ContentDatabase db(DefaultAllocator(), mount, BinarySerializerFactory(),
                                            u8".rasset");
    pipeline::ModelManifestAsset asset;
    foundation::model::ModelNode rootNode;
    rootNode.name = String(u8"Root");
    rootNode.parentIndex = -1;
    asset.manifest.nodes.PushBack(Move(rootNode));
    foundation::content::Group* group = db.RootGroup()->CreateGroup(u8"Rock");
    foundation::content::Instance* manifest =
        group->CreateInstance(u8"Rock", pipeline::ModelManifestAsset::StaticType());
    REQUIRE(manifest != nullptr);
    REQUIRE(manifest->WriteObject(asset).IsOk());

    // The defaults (no options): the prefab, no scene.
    pipeline::ModelPrefabResult prefab;
    pipeline::ModelPrefabResult generatedScene;
    REQUIRE(pipeline::GenerateForImport(DefaultAllocator(), *manifest, nullptr, prefab,
                                        generatedScene));
    REQUIRE(prefab.instance != nullptr);
    CHECK(prefab.instance->Name() == StringView(u8"Prefab"));
    CHECK(generatedScene.instance == nullptr);

    // The options say: no prefab, a scene.
    pipeline::ModelImportOptions options;
    options.generatePrefab = false;
    options.generateScene = true;
    REQUIRE(pipeline::GenerateForImport(DefaultAllocator(), *manifest, &options, prefab,
                                        generatedScene));
    CHECK(prefab.instance == nullptr);
    REQUIRE(generatedScene.instance != nullptr);
    CHECK(generatedScene.instance->Name() == StringView(u8"Scene"));

    // Not a model: nothing.
    CHECK_FALSE(pipeline::GenerateForImport(DefaultAllocator(), *generatedScene.instance, nullptr,
                                            prefab, generatedScene));
    RemoveTreeMP(dir);
}

// Sedulous a0941c27: the animator starts on the model's idle, found among the manifest's sibling
// clips by name, not on whichever clip sorts first (a kit's "Death" or "Bite_Front").
TEST_CASE("model-prefab: the animator starts on the model's idle")
{
    pipeline::RegisterModelManifestAsset();
    const StringView dir = u8"scratch_model_resting_clip_db";
    RemoveTreeMP(dir);
    (void)CreateDirectory(dir);
    foundation::vfs::NativeFileSystem mount(dir, DefaultAllocator());
    foundation::content::ContentDatabase db(DefaultAllocator(), mount, BinarySerializerFactory(),
                                            u8".rasset");
    foundation::content::Group* group = db.RootGroup()->CreateGroup(u8"Hero");
    foundation::content::Instance* manifest =
        group->CreateInstance(u8"Hero", pipeline::ModelManifestAsset::StaticType());
    REQUIRE(manifest != nullptr);
    pipeline::ModelManifestAsset asset;
    const StringView names[] = {u8"Death", u8"Idle_Gun", u8"idle"};
    for (StringView name : names)
    {
        // Any type stands in for a clip: the name is what is read.
        foundation::content::Instance* clip =
            group->CreateInstance(name, pipeline::ModelManifestAsset::StaticType());
        REQUIRE(clip != nullptr);
        asset.manifest.animationGuids.PushBack(clip->Id());
    }
    CHECK(pipeline::RestingClip(*manifest, asset.manifest) == 2u); // the exact name, any case
    asset.manifest.animationGuids.RemoveAt(2);
    CHECK(pipeline::RestingClip(*manifest, asset.manifest) == 1u); // else one containing it
    asset.manifest.animationGuids.RemoveAt(1);
    CHECK(pipeline::RestingClip(*manifest, asset.manifest) == 0u); // else the first
    RemoveTreeMP(dir);
}

TEST_CASE("model-prefab: a skinned mesh sits at identity under the skeleton's parent node")
{
    // A skin draws in its skeleton's parent's space and glTF ignores a skinned mesh node's own
    // transform: the prefab puts the mesh entity under that parent at identity, so its world is
    // the skeleton's model space (inverse-kinematics.md P0a). Nodes: Root; Armature (moved and
    // turned, the skeleton's parent); Hips (a joint); Skin (the skinned mesh, a sibling of the
    // armature carrying an offset the file says to ignore); Rig (a node under the mesh).
    pipeline::RegisterModelManifestAsset();
    engine::render::RegisterRenderComponentReflection();
    engine::animation::RegisterAnimationComponentReflection();
    GlobalTypeRegistry().Register(scene::PrefabDocument::StaticType());
    RegisterSerializable<scene::PrefabDocument>();

    const StringView dir = u8"scratch_model_prefab_skeleton_space";
    RemoveTreeMP(dir);
    (void)CreateDirectory(dir);
    foundation::vfs::NativeFileSystem mount(dir, foundation::core::DefaultAllocator());
    foundation::content::ContentDatabase db(foundation::core::DefaultAllocator(), mount,
                                            BinarySerializerFactory(), u8".rasset");

    const Guid meshSkinned{0x52, 0x7};
    const Quaternion armatureTurn = Quaternion::FromAxisAngle(Float3{0.0f, 1.0f, 0.0f}, 1.5707964f);
    usize generation = 0;
    // Generates and spawns the model with `skeletonParentNode`; returns the skinned mesh entity.
    auto spawn = [&](scene::Scene& level, i32 skeletonParentNode) -> scene::EntityHandle
    {
        pipeline::ModelManifestAsset asset;
        asset.manifest.meshGuids.PushBack(meshSkinned);
        asset.manifest.meshSkinned.PushBack(1);
        asset.manifest.meshMaterial.PushBack(-1);
        asset.manifest.meshMaterialSlots.PushBack(foundation::model::ModelMeshMaterialSlots{});
        asset.manifest.skeletonGuid = Guid{0x71, 0x7};
        asset.manifest.animationGuids.PushBack(Guid{0x72, 0x7});
        asset.manifest.skeletonParentNode = skeletonParentNode;
        auto node = [&](StringView name, i32 parent, i32 mesh, const Transform& local)
        {
            foundation::model::ModelNode n;
            n.name = String(name);
            n.parentIndex = parent;
            n.meshIndex = mesh;
            n.localTransform = local;
            asset.manifest.nodes.PushBack(Move(n));
        };
        Transform armature;
        armature.position = Float3{2.0f, 0.0f, 1.0f};
        armature.rotation = armatureTurn;
        Transform offset;
        offset.position = Float3{5.0f, 5.0f, 5.0f};
        node(u8"Root", -1, -1, Transform{});
        node(u8"Armature", 0, -1, armature);
        node(u8"Hips", 1, -1, Transform{});
        node(u8"Skin", 0, 0, offset);
        node(u8"Rig", 3, -1, Transform{});

        const String name = Format(u8"Rider{}", generation++);
        foundation::content::Group* group = db.RootGroup()->CreateGroup(name.AsView());
        REQUIRE(group != nullptr);
        foundation::content::Instance* manifestInst =
            group->CreateInstance(name.AsView(), pipeline::ModelManifestAsset::StaticType());
        REQUIRE(manifestInst != nullptr);
        REQUIRE(manifestInst->WriteObject(asset).IsOk());
        pipeline::ModelPrefabResult generated =
            pipeline::GenerateModelPrefab(DefaultAllocator(), *manifestInst);
        REQUIRE(generated.instance != nullptr);
        UniquePtr<IStream> payload = generated.instance->ReadData(u8"scene");
        REQUIRE(payload.Get() != nullptr);
        level.AddSystem<engine::render::MeshComponentManager>();
        level.AddSystem<engine::animation::SkeletalAnimationComponentManager>();
        const scene::EntityHandle root = scene::SpawnPrefab(level, *payload, generated.instance->Id());
        REQUIRE(root.IsAssigned());
        level.UpdateTransforms();
        const scene::EntityHandle skin = level.FindEntityByName(u8"Skin");
        REQUIRE(skin.IsAssigned());
        return skin;
    };
    auto sameMatrix = [](const Float4x4& a, const Float4x4& b)
    {
        for (usize r = 0; r < 4; ++r)
        {
            for (usize c = 0; c < 4; ++c)
            {
                if (std::abs(a.m[r][c] - b.m[r][c]) > 1e-5f)
                {
                    return false;
                }
            }
        }
        return true;
    };

    SUBCASE("under the armature: the mesh's world is the armature's")
    {
        scene::Scene level(DefaultAllocator(), u8"level");
        const scene::EntityHandle skin = spawn(level, 1);
        const scene::EntityHandle armature = level.FindEntityByName(u8"Armature");
        CHECK(level.GetParent(skin) == armature);
        const Transform local = level.GetLocalTransform(skin);
        CHECK(local.position.x == 0.0f);
        CHECK(local.position.y == 0.0f);
        CHECK(local.position.z == 0.0f);
        CHECK(sameMatrix(level.GetWorldMatrix(skin), level.GetWorldMatrix(armature)));
        // The armature itself keeps the file's placement.
        CHECK(level.GetLocalTransform(armature).position.x == 2.0f);
    }
    SUBCASE("a skeleton with no parent: the mesh sits on the prefab root")
    {
        scene::Scene level(DefaultAllocator(), u8"level");
        const scene::EntityHandle skin = spawn(level, -1);
        const scene::EntityHandle root = level.GetParent(level.FindEntityByName(u8"Root"));
        CHECK(level.GetParent(skin) == root);
        CHECK(level.GetLocalTransform(skin).position.y == 0.0f);
    }
    SUBCASE("a manifest from before the parent was recorded keeps the file's placement")
    {
        scene::Scene level(DefaultAllocator(), u8"level");
        const scene::EntityHandle skin = spawn(level, -2);
        CHECK(level.GetParent(skin) == level.FindEntityByName(u8"Root"));
        CHECK(level.GetLocalTransform(skin).position.y == 5.0f);
    }
    SUBCASE("a parent under the mesh itself is left alone (no cycle)")
    {
        scene::Scene level(DefaultAllocator(), u8"level");
        const scene::EntityHandle skin = spawn(level, 4);
        CHECK(level.GetParent(skin) == level.FindEntityByName(u8"Root"));
        CHECK(level.GetParent(level.FindEntityByName(u8"Rig")) == skin);
    }
    RemoveTreeMP(dir);
}
