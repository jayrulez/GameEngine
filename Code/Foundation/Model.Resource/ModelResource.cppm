// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Model.Resource - the `foundation.model.resource` module.
//
// The RUNTIME side of the cooked model family: ModelNode + ModelManifestSource (the
// cooked manifest tying mesh/material/skeleton/animation guids + node hierarchy
// together), the ModelResource runtime composite + ModelFactory, and the family-wide
// type registration. Moved OUT of modelimporter (tooling) - the player/runtime
// must not link importer/editor libraries for its cooked types.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.model.resource;

import foundation.core;
import foundation.geometry;
import foundation.geometry.resource;
import foundation.materials;
import foundation.materials.resource;
import foundation.texture;
import foundation.texture.resource;
import foundation.animation;
import foundation.animation.resource;
import foundation.resource;
import foundation.content;

using namespace foundation::core;
using namespace foundation::resource;
namespace geometry = foundation::geometry;
namespace materials = foundation::materials;
namespace texture = foundation::texture;
namespace animation = foundation::animation;

export namespace foundation::model
{

    // One node of the imported hierarchy: local TRS + an optional mesh reference (index into
    // the manifest's mesh list, -1 = no mesh). Plain copyable struct (free Serialize via ADL).
    struct ModelNode
    {
        String name;
        i32 parentIndex = -1;
        Transform localTransform;
        i32 meshIndex = -1;
    };

    // Free Serialize for ModelNode (ADL-found by the generic Array<T> serializer).
    inline void Serialize(ISerializer& ar, ModelNode& n)
    {
        foundation::core::Serialize(ar, "name", n.name);
        foundation::core::Serialize(ar, "parent", n.parentIndex);
        foundation::core::Serialize(ar, "t", n.localTransform.position);
        foundation::core::Serialize(ar, "r", n.localTransform.rotation);
        foundation::core::Serialize(ar, "s", n.localTransform.scale);
        foundation::core::Serialize(ar, "mesh", n.meshIndex);
    }

    // Authored/cooked manifest: the leaf resource Guids + the node hierarchy.
    // The material slots one mesh uses (model-wide indices into materialGuids, in the order
    // its cooked submeshes index them). Plain struct, free Serialize via ADL.
    struct ModelMeshMaterialSlots
    {
        Array<i32> slots;
    };
    inline void Serialize(ISerializer& ar, ModelMeshMaterialSlots& m)
    {
        ar.BeginObject();
        foundation::core::Serialize(ar, "slots", m.slots);
        ar.EndObject();
    }

    class ModelManifestSource final : public ISerializable
    {
        RTTI_OBJECT(ModelManifestSource, ISerializable)
    public:
        Array<Guid> meshGuids;   // cooked mesh resources
        Array<u8> meshSkinned;   // 1 if the mesh is skinned (parallel to meshGuids)
        Array<i32> meshMaterial; // material index per mesh (-1 = none); parallel to meshGuids
        // v2: the material slots per mesh (parallel to meshGuids; a cooked submesh's
        // materialIndex is a position in its entry). Empty entry = the mesh has no parts.
        Array<ModelMeshMaterialSlots> meshMaterialSlots;
        Array<Guid>
            collisionGuids; // cooked collision shape per mesh (nil = none); parallel to meshGuids
        Array<Guid> materialGuids; // cooked material resources
        Array<Guid>
            materialAlbedo; // albedo texture per material (nil = none); parallel to materialGuids
        Array<ModelNode> nodes;     // node hierarchy
        Guid skeletonGuid;          // cooked skeleton (nil if the model has no skin)
        // v3: the node the skeleton hangs from (the skin's root joint's parent: Blender's
        // armature object), -1 for the scene root, -2 unknown (no skin, or data before v3). A
        // skin draws in this node's space, so the prefab puts each skinned mesh here at identity
        // (glTF ignores a skinned mesh node's own transform; inverse-kinematics.md P0a).
        i32 skeletonParentNode = -2;
        Array<Guid> animationGuids; // cooked animation clips
        Float3 boundsMin{};         // model-space AABB (for spawn-time auto-fit/placement)
        Float3 boundsMax{};

        void Serialize(ISerializer& ar) override
        {
            foundation::core::Serialize(ar, "meshGuids", meshGuids);
            foundation::core::Serialize(ar, "meshSkinned", meshSkinned);
            foundation::core::Serialize(ar, "meshMaterial", meshMaterial);
            // Data version 2 (the enclosing payload's: ModelManifestAsset in the source DB,
            // this type in the cooked DB - both 2). A v1 asset reads without the slots and
            // the prefab builder falls back to the whole list, which matches its v1-cooked
            // meshes' model-wide submesh indices.
            if (ar.Mode() == SerializeMode::Write || ar.Version() >= 2)
            {
                foundation::core::Serialize(ar, "meshMaterialSlots", meshMaterialSlots);
            }
            foundation::core::Serialize(ar, "collisionGuids", collisionGuids);
            foundation::core::Serialize(ar, "materialGuids", materialGuids);
            foundation::core::Serialize(ar, "materialAlbedo", materialAlbedo);
            foundation::core::Serialize(ar, "nodes", nodes);
            foundation::core::Serialize(ar, "skeletonGuid", skeletonGuid);
            if (ar.Mode() == SerializeMode::Write || ar.Version() >= 3)
            {
                foundation::core::Serialize(ar, "skeletonParentNode", skeletonParentNode);
            }
            foundation::core::Serialize(ar, "animationGuids", animationGuids);
            foundation::core::Serialize(ar, "boundsMin", boundsMin);
            foundation::core::Serialize(ar, "boundsMax", boundsMax);
        }
    };

    // Runtime model: the node hierarchy + the resolved mesh proxies (parallel to the manifest
    // mesh list). The owning ResourceManager keeps the leaf resources alive via the recorded
    // dependency edges; the proxies see reloads transparently.
    class ModelResource final : public Object
    {
        RTTI_OBJECT(ModelResource, Object)
    public:
        Array<ModelNode> nodes;
        Array<RefPtr<geometry::StaticMesh>>
            meshes; // base ptr (a SkinnedMesh upcasts here); skinned via meshSkinned
        Array<u8> meshSkinned;
        Array<i32> meshMaterial; // material index per mesh (-1 = none)
        // Per mesh, the model-wide materials its submeshes index (a cooked SubMesh::materialIndex
        // is a position in its mesh's list, not in `materials`). Empty for a mesh of a v1 manifest,
        // whose submeshes index `materials` directly.
        Array<ModelMeshMaterialSlots> meshMaterialSlots;
        Array<Proxy<materials::Material>>
            materials;                       // resolved materials (albedo wired as default texture)
        Proxy<animation::Skeleton> skeleton; // resolved skeleton (null if not skinned)
        Array<Proxy<animation::AnimationClip>> animations; // resolved animation clips
        Float3 boundsMin{};
        Float3 boundsMax{};

        // The model-wide material indices (into `materials`) a mesh's submeshes index, in slot
        // order: what an entity drawing the mesh binds as its material list. A v1 manifest's mesh
        // (no slots) indexes every material, in order.
        void MeshMaterialIndices(usize meshIndex, Array<i32>& out) const
        {
            out.Clear();
            if (meshIndex < meshMaterialSlots.Size() && !meshMaterialSlots[meshIndex].slots.IsEmpty())
            {
                for (const i32 slot : meshMaterialSlots[meshIndex].slots)
                {
                    out.PushBack(slot);
                }
                return;
            }
            for (usize i = 0; i < materials.Size(); ++i)
            {
                out.PushBack(static_cast<i32>(i));
            }
        }
    };

    // Builds a ModelResource from a ModelManifestSource: copies the hierarchy + resolves each
    // mesh Guid via manager.Bind (recording the model->mesh dependency edge).
    class ModelFactory final : public IResourceFactory
    {
    public:
        [[nodiscard]] const TypeInfo* ProductType() const override
        {
            return &ModelResource::StaticType();
        }
        [[nodiscard]] const TypeInfo* CookedType() const override
        {
            return &ModelManifestSource::StaticType();
        }

        [[nodiscard]] RefPtr<Object> Create(ResourceManager& manager,
                                            foundation::content::Instance& instance) override
        {
            RefPtr<ISerializable> object = instance.ReadObject();
            ModelManifestSource* src = Cast<ModelManifestSource>(object.Get());
            if (src == nullptr)
            {
                return RefPtr<Object>{};
            }

            RefPtr<ModelResource> model = MakeRef<ModelResource>(DefaultAllocator());
            for (const ModelNode& n : src->nodes)
            {
                model->nodes.PushBack(n);
            }
            for (const u8 s : src->meshSkinned)
            {
                model->meshSkinned.PushBack(s);
            }
            for (const i32 m : src->meshMaterial)
            {
                model->meshMaterial.PushBack(m);
            }
            for (const ModelMeshMaterialSlots& slots : src->meshMaterialSlots)
            {
                model->meshMaterialSlots.PushBack(slots);
            }

            // Resolve meshes (skinned ones bind as SkinnedMesh, stored as the StaticMesh base; the renderer
            // checks IsSkinned() + uploads the skin stream). Bind records the model->mesh dependency edge.
            for (usize i = 0; i < src->meshGuids.Size(); ++i)
            {
                const bool skinned = (i < src->meshSkinned.Size() && src->meshSkinned[i] != 0);
                geometry::StaticMesh* mesh =
                    skinned ? static_cast<geometry::StaticMesh*>(
                                  manager.Bind<geometry::SkinnedMesh>(src->meshGuids[i]).Get())
                            : manager.Bind<geometry::StaticMesh>(src->meshGuids[i]).Get();
                model->meshes.PushBack(RefPtr<geometry::StaticMesh>(mesh));
            }

            // Resolve the skeleton + animation clips (composite edges).
            if (!src->skeletonGuid.IsNil())
            {
                model->skeleton = manager.Bind<animation::Skeleton>(src->skeletonGuid);
            }
            for (const Guid& g : src->animationGuids)
            {
                model->animations.PushBack(manager.Bind<animation::AnimationClip>(g));
            }

            // Resolve materials + wire their albedo texture in as the material's default (the renderer's
            // per-material instance reads default textures, so no per-instance assignment is needed).
            for (usize i = 0; i < src->materialGuids.Size(); ++i)
            {
                Proxy<materials::Material> material =
                    manager.Bind<materials::Material>(src->materialGuids[i]);
                if (material && i < src->materialAlbedo.Size() && !src->materialAlbedo[i].IsNil())
                {
                    Proxy<texture::Texture> albedo =
                        manager.Bind<texture::Texture>(src->materialAlbedo[i]);
                    if (albedo && albedo->View() != nullptr)
                    {
                        material->SetDefaultTexture(u8"AlbedoMap", albedo->View());
                    }
                }
                model->materials.PushBack(material);
            }

            model->boundsMin = src->boundsMin;
            model->boundsMax = src->boundsMax;
            return model;
        }
    };

    RTTI_DEFINE_OBJECT(ModelManifestSource, "rtti::modelimporter")
    RTTI_DEFINE_OBJECT(ModelResource, "rtti::modelimporter")

    // Registers the importer's serializable types (+ the geometry source types it cooks and
    // reads back) with the global type + serializable registries. Registration is explicit
    // (RTTI_DEFINE_OBJECT only defines StaticType); call this once before binding cooked
    // models so the content DB can polymorphically deserialize them. Idempotent.
    // Registers the whole cooked-model family's product/source types (model manifest +
    // mesh/material/texture/animation) for by-type-name construction at runtime.
    inline void RegisterModelResourceTypes()
    {
        // Data version 3: the skeleton's parent node; a v2 cooked manifest still reads (no
        // parent known, the prefab keeps the file's placement). v0 and v1 re-cook: the strict
        // reader refuses them. No REFLECT block owns this type - patched on the TypeInfo.
        const_cast<TypeInfo&>(ModelManifestSource::StaticType()).dataVersion = 3;
        const_cast<TypeInfo&>(ModelManifestSource::StaticType()).minReadDataVersion = 2;
        GlobalTypeRegistry().Register(ModelManifestSource::StaticType());
        RegisterSerializable<ModelManifestSource>();
        GlobalTypeRegistry().Register(ModelResource::StaticType());

        // The sibling libraries register their own (engine-composition.md D1); a caller that
        // reaches this family through the model keeps seeing all of it.
        geometry::RegisterGeometryResourceTypes();
        materials::RegisterMaterialResourceTypes();
        texture::RegisterTextureResource();
        animation::RegisterAnimationResourceTypes();
    }

} // namespace foundation::model

export namespace foundation::model
{
    /// The model resource module (engine-composition.md D1): the module the engine
    /// composition composes this library's factories from.
    inline constexpr foundation::resource::ResourceFactoryDesc kModelResourceFactories[] = {
        foundation::resource::FactoryByDefault<ModelResource, ModelManifestSource, ModelFactory>(),
    };
    inline constexpr foundation::resource::ResourceModule kModelResourceModule{
        u8"model", &RegisterModelResourceTypes, kModelResourceFactories,
        sizeof(kModelResourceFactories) / sizeof(kModelResourceFactories[0])};
}
