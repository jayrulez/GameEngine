// Draconic::ModelResource - the `draconic.model.resource` module.
//
// The RUNTIME side of the cooked model family: ModelNode + ModelManifestSource (the
// cooked manifest tying mesh/material/skeleton/animation guids + node hierarchy
// together), the ModelResource runtime composite + ModelFactory, and the family-wide
// type registration. Moved OUT of draconic.modelimporter (tooling) - the player/runtime
// must not link importer/editor libraries for its cooked types.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.model.resource;

import draconic.foundation;
import draconic.geometry;
import draconic.geometry.resource;
import draconic.materials;
import draconic.materials.resource;
import draconic.texture;
import draconic.texture.resource;
import draconic.animation;
import draconic.animation.resource;
import draconic.resource;
import draconic.content;

using namespace draconic::foundation;
using namespace draconic::resource;
namespace geometry = draconic::geometry;
namespace materials = draconic::materials;
namespace texture = draconic::texture;
namespace animation = draconic::animation;

export namespace draconic::model
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
        draconic::foundation::Serialize(ar, "name", n.name);
        draconic::foundation::Serialize(ar, "parent", n.parentIndex);
        draconic::foundation::Serialize(ar, "t", n.localTransform.position);
        draconic::foundation::Serialize(ar, "r", n.localTransform.rotation);
        draconic::foundation::Serialize(ar, "s", n.localTransform.scale);
        draconic::foundation::Serialize(ar, "mesh", n.meshIndex);
    }

    // Authored/cooked manifest: the leaf resource Guids + the node hierarchy.
    class ModelManifestSource final : public ISerializable
    {
        DRACONIC_OBJECT(ModelManifestSource, ISerializable)
    public:
        Array<Guid> meshGuids;   // cooked mesh resources
        Array<u8> meshSkinned;   // 1 if the mesh is skinned (parallel to meshGuids)
        Array<i32> meshMaterial; // material index per mesh (-1 = none); parallel to meshGuids
        Array<Guid>
            collisionGuids; // cooked collision shape per mesh (nil = none); parallel to meshGuids
        Array<Guid> materialGuids; // cooked material resources
        Array<Guid>
            materialAlbedo; // albedo texture per material (nil = none); parallel to materialGuids
        Array<ModelNode> nodes;     // node hierarchy
        Guid skeletonGuid;          // cooked skeleton (nil if the model has no skin)
        Array<Guid> animationGuids; // cooked animation clips
        Float3 boundsMin{};         // model-space AABB (for spawn-time auto-fit/placement)
        Float3 boundsMax{};

        void Serialize(ISerializer& ar) override
        {
            draconic::foundation::Serialize(ar, "meshGuids", meshGuids);
            draconic::foundation::Serialize(ar, "meshSkinned", meshSkinned);
            draconic::foundation::Serialize(ar, "meshMaterial", meshMaterial);
            draconic::foundation::Serialize(ar, "collisionGuids", collisionGuids);
            draconic::foundation::Serialize(ar, "materialGuids", materialGuids);
            draconic::foundation::Serialize(ar, "materialAlbedo", materialAlbedo);
            draconic::foundation::Serialize(ar, "nodes", nodes);
            draconic::foundation::Serialize(ar, "skeletonGuid", skeletonGuid);
            draconic::foundation::Serialize(ar, "animationGuids", animationGuids);
            draconic::foundation::Serialize(ar, "boundsMin", boundsMin);
            draconic::foundation::Serialize(ar, "boundsMax", boundsMax);
        }
    };

    // Runtime model: the node hierarchy + the resolved mesh proxies (parallel to the manifest
    // mesh list). The owning ResourceManager keeps the leaf resources alive via the recorded
    // dependency edges; the proxies see reloads transparently.
    class ModelResource final : public Object
    {
        DRACONIC_OBJECT(ModelResource, Object)
    public:
        Array<ModelNode> nodes;
        Array<RefPtr<geometry::StaticMesh>>
            meshes; // base ptr (a SkinnedMesh upcasts here); skinned via meshSkinned
        Array<u8> meshSkinned;
        Array<i32> meshMaterial; // material index per mesh (-1 = none)
        Array<Proxy<materials::Material>>
            materials;                       // resolved materials (albedo wired as default texture)
        Proxy<animation::Skeleton> skeleton; // resolved skeleton (null if not skinned)
        Array<Proxy<animation::AnimationClip>> animations; // resolved animation clips
        Float3 boundsMin{};
        Float3 boundsMax{};
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

        [[nodiscard]] RefPtr<Object> Create(ResourceManager& manager,
                                            draconic::content::Instance& instance) override
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

    DRACONIC_DEFINE_OBJECT(ModelManifestSource, "draconic::modelimporter")
    DRACONIC_DEFINE_OBJECT(ModelResource, "draconic::modelimporter")

    // Registers the importer's serializable types (+ the geometry source types it cooks and
    // reads back) with the global type + serializable registries. Registration is explicit in
    // Draconic (DRACONIC_DEFINE_OBJECT only defines StaticType); call this once before binding cooked
    // models so the content DB can polymorphically deserialize them. Idempotent.
    // Registers the whole cooked-model family's product/source types (model manifest +
    // mesh/material/texture/animation) for by-type-name construction at runtime.
    inline void RegisterModelResourceTypes()
    {
        GlobalTypeRegistry().Register(ModelManifestSource::StaticType());
        RegisterSerializable<ModelManifestSource>();
        GlobalTypeRegistry().Register(ModelResource::StaticType());

        GlobalTypeRegistry().Register(geometry::StaticMeshSource::StaticType());
        RegisterSerializable<geometry::StaticMeshSource>();
        GlobalTypeRegistry().Register(geometry::SkinnedMeshSource::StaticType());
        RegisterSerializable<geometry::SkinnedMeshSource>();
        GlobalTypeRegistry().Register(geometry::StaticMesh::StaticType());
        GlobalTypeRegistry().Register(geometry::SkinnedMesh::StaticType());
        RegisterSerializable<geometry::SkinnedMeshSource>();

        GlobalTypeRegistry().Register(materials::MaterialSource::StaticType());
        RegisterSerializable<materials::MaterialSource>();
        GlobalTypeRegistry().Register(materials::Material::StaticType());

        GlobalTypeRegistry().Register(texture::TextureResource::StaticType());
        RegisterSerializable<texture::TextureResource>();
        GlobalTypeRegistry().Register(texture::Texture::StaticType());

        GlobalTypeRegistry().Register(animation::SkeletonSource::StaticType());
        RegisterSerializable<animation::SkeletonSource>();
        GlobalTypeRegistry().Register(animation::Skeleton::StaticType());
        GlobalTypeRegistry().Register(animation::AnimationClipSource::StaticType());
        RegisterSerializable<animation::AnimationClipSource>();
        GlobalTypeRegistry().Register(animation::AnimationClip::StaticType());
    }

} // namespace draconic::model
