// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Physics.Resource - the `foundation.physics.resource` module.
//
// Cooked physics content, mirroring the mesh-resource split:
//   * CollisionShapeSource  - the cooked record: a Jolt binary shape blob (from
//     CookConvexHull or CookTriangleMesh) and cached debug-outline triangles (gizmos draw
//     without restoring the shape).
//   * CollisionShape        - the runtime product a component's Ref<> binds; hands the
//     blob to PhysicsWorld as ShapeKind::Cooked.
//   * PhysicalMaterialSource / PhysicalMaterial - surface properties a RigidBody can
//     reference instead of authoring friction and restitution inline.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.physics.resource;

import foundation.core;
import foundation.resource;
import foundation.content;

using namespace foundation::core;
namespace resource = foundation::resource;

export namespace foundation::physics
{
    // Cooked collision shape: the Jolt binary blob and debug outline (xyz triples, 3 per
    // triangle). Both produced by the CollisionShapeAssetBuilder.
    class CollisionShapeSource : public ISerializable
    {
        RTTI_OBJECT(CollisionShapeSource, ISerializable)
    public:
        bool convex = false; // informational (the blob self-describes)
        Array<u8> shapeBlob; // Jolt binary shape state
        Array<f32> outline;  // debug triangles: x,y,z per vertex, 9 floats per tri

        void Serialize(ISerializer& ar) override
        {
            foundation::core::Serialize(ar, "convex", convex);
            foundation::core::Serialize(ar, "shapeBlob", shapeBlob);
            foundation::core::Serialize(ar, "outline", outline);
        }
    };

    // Runtime product: what RigidBody and Collider components bind through Ref<CollisionShape>.
    class CollisionShape : public Object
    {
        RTTI_OBJECT(CollisionShape, Object)
    public:
        bool convex = false;
        Array<byte> blob;
        Array<Float3> outline; // debug triangles (3 vertices each)

        [[nodiscard]] Span<const byte> Blob() const noexcept
        {
            return Span<const byte>(blob.Data(), blob.Size());
        }
    };

    class CollisionShapeFactory final : public resource::IResourceFactory
    {
    public:
        [[nodiscard]] const TypeInfo* ProductType() const override
        {
            return &CollisionShape::StaticType();
        }
        [[nodiscard]] const TypeInfo* CookedType() const override
        {
            return &CollisionShapeSource::StaticType();
        }
        [[nodiscard]] RefPtr<Object> Create(resource::ResourceManager&,
                                            foundation::content::Instance& instance) override
        {
            RefPtr<ISerializable> object = instance.ReadObject();
            CollisionShapeSource* source = Cast<CollisionShapeSource>(object.Get());
            if (source == nullptr)
            {
                return RefPtr<Object>{};
            }
            RefPtr<CollisionShape> shape = MakeRef<CollisionShape>(DefaultAllocator());
            shape->convex = source->convex;
            shape->blob.Resize(source->shapeBlob.Size());
            if (!source->shapeBlob.IsEmpty())
            {
                MemCopy(shape->blob.Data(), source->shapeBlob.Data(), source->shapeBlob.Size());
            }
            const usize vertexCount = source->outline.Size() / 3;
            shape->outline.Reserve(vertexCount);
            for (usize v = 0; v < vertexCount; ++v)
            {
                shape->outline.PushBack(Float3{source->outline[v * 3 + 0],
                                               source->outline[v * 3 + 1],
                                               source->outline[v * 3 + 2]});
            }
            return shape;
        }
    };

    // Surface properties; referenced by RigidBodyComponents (overrides the inline fields).
    class PhysicalMaterialSource : public ISerializable
    {
        RTTI_OBJECT(PhysicalMaterialSource, ISerializable)
    public:
        f32 friction = 0.5f;
        f32 restitution = 0.0f;
        f32 density = 1000.0f; // kg/m^3

        void Serialize(ISerializer& ar) override
        {
            foundation::core::Serialize(ar, "friction", friction);
            foundation::core::Serialize(ar, "restitution", restitution);
            foundation::core::Serialize(ar, "density", density);
        }
    };

    class PhysicalMaterial : public Object
    {
        RTTI_OBJECT(PhysicalMaterial, Object)
    public:
        f32 friction = 0.5f;
        f32 restitution = 0.0f;
        f32 density = 1000.0f;
    };

    class PhysicalMaterialFactory final : public resource::IResourceFactory
    {
    public:
        [[nodiscard]] const TypeInfo* ProductType() const override
        {
            return &PhysicalMaterial::StaticType();
        }
        [[nodiscard]] const TypeInfo* CookedType() const override
        {
            return &PhysicalMaterialSource::StaticType();
        }
        [[nodiscard]] RefPtr<Object> Create(resource::ResourceManager&,
                                            foundation::content::Instance& instance) override
        {
            RefPtr<ISerializable> object = instance.ReadObject();
            PhysicalMaterialSource* source = Cast<PhysicalMaterialSource>(object.Get());
            if (source == nullptr)
            {
                return RefPtr<Object>{};
            }
            RefPtr<PhysicalMaterial> material = MakeRef<PhysicalMaterial>(DefaultAllocator());
            material->friction = source->friction;
            material->restitution = source->restitution;
            material->density = source->density;
            return material;
        }
    };

    // Registers the cooked record and product types (content-database construction by type name).
    inline void RegisterPhysicsResource()
    {
        GlobalTypeRegistry().Register(CollisionShapeSource::StaticType());
        RegisterSerializable<CollisionShapeSource>();
        GlobalTypeRegistry().Register(CollisionShape::StaticType());
        GlobalTypeRegistry().Register(PhysicalMaterialSource::StaticType());
        RegisterSerializable<PhysicalMaterialSource>();
        GlobalTypeRegistry().Register(PhysicalMaterial::StaticType());
    }

    RTTI_DEFINE_OBJECT(CollisionShapeSource, "rtti::physics")
    RTTI_DEFINE_OBJECT(CollisionShape, "rtti::physics")
    RTTI_DEFINE_OBJECT(PhysicalMaterialSource, "rtti::physics")
    RTTI_DEFINE_OBJECT(PhysicalMaterial, "rtti::physics")
}

export namespace foundation::physics
{
    /// The physics resource module: what the engine composition builds this library's
    /// factories from.
    inline constexpr foundation::resource::ResourceFactoryDesc kPhysicsResourceFactories[] = {
        foundation::resource::FactoryByDefault<CollisionShape, CollisionShapeSource, CollisionShapeFactory>(),
        foundation::resource::FactoryByDefault<PhysicalMaterial, PhysicalMaterialSource, PhysicalMaterialFactory>(),
    };
    inline constexpr foundation::resource::ResourceModule kPhysicsResourceModule{
        u8"physics", &RegisterPhysicsResource, kPhysicsResourceFactories,
        sizeof(kPhysicsResourceFactories) / sizeof(kPhysicsResourceFactories[0])};
}
