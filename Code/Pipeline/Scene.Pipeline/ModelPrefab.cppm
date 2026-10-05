// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Pipeline::Scene - :model_prefab partition.
//
// Model->prefab generation: a model import's manifest (node hierarchy + cooked
// leaf guids) becomes a spawnable PrefabDocument named "Prefab" inside the model's group. The
// wiring mirrors the runtime model spawn (Sandbox): one entity per node, MeshComponents with
// mesh/material refs by guid (per-submesh refs for multi-material models), and - when the model
// brought a skeleton + clips - ONE SkeletalAnimationComponent on the root whose meshEntities
// (EntityRefs, prefab-remapped per spawn) feed every skinned mesh node.
//
// RE-IMPORT REGENERATES: the "Prefab" instance is found by name and reused (same guid), so
// placed instances rebuild through the standard prefab machinery. This lives in the scene
// domain's pipeline library - not the importer - because it needs scene + component machinery
// the importer library (linked by the headless cooker) deliberately never links. Every host runs
// it once an import has landed (GenerateForImport): the editor through its import listener,
// which adds its own effects, the stdio MCP host through pipeline::AfterImport.
//
// GenerateModelScene is the twin: the SAME BuildModelScene hierarchy saved as a standalone
// SceneDocument named "Scene" (a bare scene - model nodes only, no default camera/light) when
// the import's "Generate scene" toggle is on. Prefab + scene are independent toggles.

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"

export module scene.pipeline:model_prefab;

import foundation.core;
import foundation.content;
import foundation.resource;
import foundation.materials;
import foundation.scene;
import foundation.scene.resource;
import engine.render;
import engine.animation;
import foundation.physics;
import engine.physics;
import modelimporter;
import pipeline.importer;

using namespace foundation::core;

export namespace pipeline
{
    namespace scene = foundation::scene;
    namespace render = foundation::render;

    struct ModelPrefabResult
    {
        foundation::content::Instance* instance = nullptr;
        bool regenerated = false; // an existing prefab was refreshed (re-import)
    };

    /// The clip a model's animator starts on: its idle when it has one (a clip named Idle, any
    /// case, else one whose name contains it), else its first. The clips are the manifest's
    /// siblings; their names are the importer's, from the source's animation names. A kit
    /// exported in name order starts on 'Death' or 'Bite_Front' otherwise.
    [[nodiscard]] inline usize RestingClip(foundation::content::Instance& manifestInstance,
                                           const foundation::model::ModelManifestSource& manifest)
    {
        usize containing = manifest.animationGuids.Size();
        for (usize i = 0; i < manifest.animationGuids.Size(); ++i)
        {
            for (foundation::content::Instance* sibling : manifestInstance.OwningGroup().Instances())
            {
                if (sibling->Id() != manifest.animationGuids[i])
                {
                    continue;
                }
                const StringView name = sibling->Name();
                if (name.Size() == 4 && name.ContainsIgnoreCase(u8"idle"))
                {
                    return i;
                }
                if (containing == manifest.animationGuids.Size() && name.ContainsIgnoreCase(u8"idle"))
                {
                    containing = i;
                }
            }
        }
        return containing < manifest.animationGuids.Size() ? containing : 0;
    }

    /// A skin draws in its skeleton's parent's space (the joints' palette is relative to it), and
    /// glTF ignores a skinned mesh node's own transform: so the skinned mesh entity goes under
    /// the skeleton's parent node (the prefab root when the skeleton has none) at identity, and
    /// its world IS the skeleton's model space wherever the file put the mesh node
    /// (inverse-kinematics.md P0a). Left where the file put it when the manifest does not know
    /// the parent (before data version 3) or that parent is the mesh node or below it.
    inline void PlaceInSkeletonSpace(scene::Scene& scene,
                                     const foundation::model::ModelManifestSource& manifest,
                                     const Array<scene::EntityHandle>& entities,
                                     scene::EntityHandle root, usize meshNode)
    {
        const i32 parent = manifest.skeletonParentNode;
        if (parent < -1 || parent >= static_cast<i32>(entities.Size()))
        {
            return;
        }
        // The walk up is bounded by the node count, so a malformed cycle cannot hang it.
        i32 node = parent;
        for (usize steps = 0; node >= 0 && static_cast<usize>(node) < manifest.nodes.Size() &&
                              steps <= manifest.nodes.Size();
             ++steps)
        {
            if (static_cast<usize>(node) == meshNode)
            {
                return;
            }
            node = manifest.nodes[static_cast<usize>(node)].parentIndex;
        }
        scene.SetParent(entities[meshNode], parent >= 0 ? entities[static_cast<usize>(parent)] : root);
        scene.SetLocalTransform(entities[meshNode], Transform{});
    }

    /// Build the model's node hierarchy into `scene` (one entity per node with mesh/material/
    /// collider/skeletal-anim components, plus a STATIC compound RigidBody on the root when the
    /// model carries collision), returning its root. Shared by the prefab + scene generators.
    [[nodiscard]] inline bool BuildModelScene(foundation::content::Instance& manifestInstance,
                                              scene::Scene& scene, scene::EntityHandle& outRoot)
    {
        RefPtr<ISerializable> object = manifestInstance.ReadObject();
        auto* asset = Cast<pipeline::ModelManifestAsset>(object.Get());
        if (asset == nullptr)
        {
            LOG_ERROR(u8"Import", u8"model prefab/scene: manifest '{}' failed to read back",
                      manifestInstance.Name());
            return false;
        }
        const foundation::model::ModelManifestSource& manifest = asset->manifest;

        auto* meshes = scene.AddSystem<engine::render::MeshComponentManager>();
        auto* anims = scene.AddSystem<engine::animation::SkeletalAnimationComponentManager>();
        const bool hasCollision = !manifest.collisionGuids.IsEmpty();
        auto* rigidBodies = hasCollision
                                ? scene.AddSystem<engine::physics::RigidBodyComponentManager>()
                                : nullptr;
        auto* colliders =
            hasCollision ? scene.AddSystem<engine::physics::ColliderComponentManager>() : nullptr;

        scene::EntityHandle root = scene.CreateEntity(manifestInstance.Name());
        Array<scene::EntityHandle> entities;
        entities.Reserve(manifest.nodes.Size());
        for (const foundation::model::ModelNode& node : manifest.nodes)
        {
            scene::EntityHandle e = scene.CreateEntity(node.name.AsView());
            scene.SetLocalTransform(e, node.localTransform);
            entities.PushBack(e);
        }
        const bool animated = !manifest.skeletonGuid.IsNil() && !manifest.animationGuids.IsEmpty();
        Array<scene::EntityHandle> skinnedEntities;
        for (usize i = 0; i < manifest.nodes.Size(); ++i)
        {
            const foundation::model::ModelNode& node = manifest.nodes[i];
            if (node.parentIndex >= 0 && static_cast<usize>(node.parentIndex) < entities.Size())
            {
                scene.SetParent(entities[i], entities[static_cast<usize>(node.parentIndex)]);
            }
            else
            {
                scene.SetParent(entities[i], root); // top-level node -> the prefab root
            }
            if (node.meshIndex < 0 ||
                static_cast<usize>(node.meshIndex) >= manifest.meshGuids.Size())
            {
                continue;
            }
            const usize meshIndex = static_cast<usize>(node.meshIndex);
            if (manifest.meshGuids[meshIndex].IsNil())
            {
                // A held slot: the mesh folded into a LOD chain or was deselected at import -
                // an empty MeshComponent would only be noise.
                continue;
            }
            engine::render::MeshComponent& mc = meshes->Add(entities[i]);
            mc.mesh.SetId(manifest.meshGuids[meshIndex]);

            // The material list, indexed by SubMesh::materialIndex (slot 0 covers
            // single-material meshes and out-of-range indexes). A v2 manifest records the
            // slots each mesh uses and its submeshes index THAT list, so the entity binds
            // only its own materials (Bistro: 254 per node before, one or two after). A v1
            // manifest (no slots) cooked its submeshes with model-wide indices: the whole
            // list stays their table.
            const bool hasSlots = meshIndex < manifest.meshMaterialSlots.Size() &&
                                  !manifest.meshMaterialSlots[meshIndex].slots.IsEmpty();
            if (hasSlots)
            {
                for (const i32 slot : manifest.meshMaterialSlots[meshIndex].slots)
                {
                    foundation::resource::Ref<foundation::materials::Material> r;
                    if (slot >= 0 && static_cast<usize>(slot) < manifest.materialGuids.Size())
                    {
                        r.SetId(manifest.materialGuids[static_cast<usize>(slot)]);
                    }
                    mc.materials.PushBack(r); // an unresolvable slot stays an empty ref
                }
            }
            else
            {
                for (const Guid& g : manifest.materialGuids)
                {
                    foundation::resource::Ref<foundation::materials::Material> r;
                    r.SetId(g);
                    mc.materials.PushBack(r);
                }
            }

            // Generated collision: each mesh node gets a cooked-shape collider that folds
            // into the STATIC RigidBody on the prefab root (hierarchy compounding).
            if (colliders != nullptr && meshIndex < manifest.collisionGuids.Size() &&
                !manifest.collisionGuids[meshIndex].IsNil())
            {
                engine::physics::ColliderComponent& cc = colliders->Add(entities[i]);
                cc.shape = foundation::physics::ShapeKind::Cooked;
                cc.collisionShape.SetId(manifest.collisionGuids[meshIndex]);
            }

            const bool skinned =
                (meshIndex < manifest.meshSkinned.Size()) && manifest.meshSkinned[meshIndex] != 0;
            if (skinned && animated)
            {
                skinnedEntities.PushBack(entities[i]);
                PlaceInSkeletonSpace(scene, manifest, entities, root, i);
            }
        }

        // ONE animator on the root drives every skinned mesh node via EntityRef (prefab
        // instancing remaps the guids per spawn). One player, one pose evaluation per frame -
        // per-part animators sampled the same clip once per part.
        if (!skinnedEntities.IsEmpty())
        {
            engine::animation::SkeletalAnimationComponent& ac = anims->Add(root);
            ac.skeleton.SetId(manifest.skeletonGuid);
            ac.clip.SetId(manifest.animationGuids[RestingClip(manifestInstance, manifest)]);
            for (scene::EntityHandle e : skinnedEntities)
            {
                ac.meshEntities.PushBack(scene.GetEntityId(e));
            }
        }

        if (rigidBodies != nullptr)
        {
            bool anyCollider = false;
            for (const Guid& g : manifest.collisionGuids)
            {
                anyCollider = anyCollider || !g.IsNil();
            }
            if (anyCollider)
            {
                engine::physics::RigidBodyComponent& body = rigidBodies->Add(root);
                body.motion = foundation::physics::MotionKind::Static;
                body.layer = foundation::physics::PhysicsLayer::Static;
                // The root body's OWN shape stays a degenerate box; the real geometry
                // comes from the descendant cooked colliders compounding in.
                body.halfExtents = Float3{0.01f, 0.01f, 0.01f};
            }
        }

        outRoot = root;
        return true;
    }

    /// Generate (or refresh) the hierarchy prefab for a model manifest instance. Pure
    /// content-DB work - the caller handles editor-side follow-ups (instance rebuild,
    /// open-page refresh, notices).
    [[nodiscard]] inline ModelPrefabResult
    GenerateModelPrefab(IAllocator& allocator, foundation::content::Instance& manifestInstance)
    {
        ModelPrefabResult result;
        scene::Scene scene(allocator, manifestInstance.Name());
        scene::EntityHandle root;
        if (!BuildModelScene(manifestInstance, scene, root))
        {
            return result;
        }

        MemoryStream payload;
        if (!scene::CapturePrefab(scene, root, payload).IsOk())
        {
            LOG_ERROR(u8"Import", u8"model prefab: capture failed for '{}'",
                      manifestInstance.Name());
            return result;
        }

        // "Prefab" beside the manifest; an existing one is REUSED so its guid (and every
        // placed instance) survives re-import. A non-prefab squatting on the name loses to
        // a suffixed fallback rather than being clobbered.
        foundation::content::Group& group = manifestInstance.OwningGroup();
        foundation::content::Instance* prefab = group.GetInstance(u8"Prefab");
        if (prefab != nullptr && prefab->TypeName() != StringView(u8"PrefabDocument"))
        {
            prefab = group.GetInstance(u8"Prefab.2");
            if (prefab == nullptr)
            {
                prefab = group.CreateInstance(u8"Prefab.2", scene::PrefabDocument::StaticType());
            }
        }
        result.regenerated = prefab != nullptr;
        if (prefab == nullptr)
        {
            prefab = group.CreateInstance(u8"Prefab", scene::PrefabDocument::StaticType());
        }
        if (prefab == nullptr)
        {
            LOG_ERROR(u8"Import",
                      u8"model prefab: could not create the 'Prefab' instance beside '{}'",
                      manifestInstance.Name());
            return result;
        }

        scene::PrefabDocument doc;
        doc.name = String(manifestInstance.Name());
        if (!prefab->WriteObject(doc).IsOk() ||
            !prefab->WriteData(u8"scene", payload.Bytes(),
                               foundation::content::StreamEncoding::Text)
                 .IsOk())
        {
            LOG_ERROR(u8"Import", u8"model prefab write failed for '{}'",
                               manifestInstance.Name());
            result.regenerated = false;
            return result;
        }
        result.instance = prefab;
        return result;
    }

    /// Generate (or refresh) a SCENE of the model's node hierarchy - the SAME contents the prefab
    /// carries, saved as a standalone SceneDocument named "Scene" beside the manifest (a bare scene:
    /// model nodes only, no default camera/light). Re-import REUSES the instance by name (same guid)
    /// so placed scene references survive.
    [[nodiscard]] inline ModelPrefabResult
    GenerateModelScene(IAllocator& allocator, foundation::content::Instance& manifestInstance)
    {
        ModelPrefabResult result;
        scene::Scene scene(allocator, manifestInstance.Name());
        scene::EntityHandle root;
        if (!BuildModelScene(manifestInstance, scene, root))
        {
            return result;
        }
        (void)root; // SaveScene captures the whole scene; the root is just its top entity

        // "Scene" beside the manifest; an existing one is REUSED so its guid (and every placed
        // reference) survives re-import. A non-scene squatting on the name loses to a suffixed
        // fallback rather than being clobbered.
        foundation::content::Group& group = manifestInstance.OwningGroup();
        foundation::content::Instance* sceneInstance = group.GetInstance(u8"Scene");
        if (sceneInstance != nullptr &&
            sceneInstance->TypeName() != StringView(u8"SceneDocument"))
        {
            sceneInstance = group.GetInstance(u8"Scene.2");
            if (sceneInstance == nullptr)
            {
                sceneInstance =
                    group.CreateInstance(u8"Scene.2", scene::SceneDocument::StaticType());
            }
        }
        result.regenerated = sceneInstance != nullptr;
        if (sceneInstance == nullptr)
        {
            sceneInstance = group.CreateInstance(u8"Scene", scene::SceneDocument::StaticType());
        }
        if (sceneInstance == nullptr)
        {
            LOG_ERROR(u8"Import",
                      u8"model scene: could not create the 'Scene' instance beside '{}'",
                      manifestInstance.Name());
            return result;
        }

        if (!scene::SaveScene(scene, *sceneInstance).IsOk())
        {
            LOG_ERROR(u8"Import", u8"model scene write failed for '{}'",
                               manifestInstance.Name());
            result.regenerated = false;
            return result;
        }
        result.instance = sceneInstance;
        return result;
    }

    /// What a finished model import generates, by its options (null is the defaults): the
    /// prefab unless "Generate prefab" is off, the scene when "Generate scene" is on. Every host
    /// runs it once an import has landed, so a model imported anywhere gets the same assets; the
    /// editor adds its own effects (refreshing placed instances) around it. False, generating
    /// nothing, when `primary` is not a model manifest. A result's instance is null when that one
    /// was not asked for, or failed (logged).
    [[nodiscard]] inline bool GenerateForImport(IAllocator& allocator,
                                                foundation::content::Instance& primary,
                                                const ImportOptions* options,
                                                ModelPrefabResult& outPrefab,
                                                ModelPrefabResult& outScene)
    {
        outPrefab = ModelPrefabResult{};
        outScene = ModelPrefabResult{};
        const StringView manifestType(
            reinterpret_cast<const utf8char*>(ModelManifestAsset::StaticType().name));
        if (primary.TypeName() != manifestType)
        {
            return false;
        }
        bool wantPrefab = true;
        bool wantScene = false;
        if (const auto* model = Cast<ModelImportOptions>(const_cast<ImportOptions*>(options)))
        {
            wantPrefab = model->generatePrefab;
            wantScene = model->generateScene;
        }
        if (wantPrefab)
        {
            outPrefab = GenerateModelPrefab(allocator, primary);
        }
        if (wantScene)
        {
            outScene = GenerateModelScene(allocator, primary);
        }
        return true;
    }
}
