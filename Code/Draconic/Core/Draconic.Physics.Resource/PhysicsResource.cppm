// Draconic::PhysicsResource - the `draconic.physics.resource` module.
//
// Cooked physics content (docs/design/physics.md §5), mirroring the mesh-resource split:
//   * CollisionShapeSource  - the cooked record: a Jolt binary shape blob (from
//     CookConvexHull/CookTriangleMesh) + cached debug-outline triangles (gizmos draw
//     without restoring the shape).
//   * CollisionShape        - the runtime product a component's Ref<> binds; hands the
//     blob to PhysicsWorld as ShapeKind::Cooked.
//   * PhysicalMaterialSource / PhysicalMaterial - surface properties a RigidBody can
//     reference instead of authoring friction/restitution inline.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.physics.resource;

import draconic.foundation;
import draconic.resource;
import draconic.content;

using namespace draconic::foundation;
namespace resource = draconic::resource;

export namespace draconic::physics
{
    // Cooked collision shape: the Jolt binary blob + debug outline (xyz triples, 3 per
    // triangle). Both produced by the CollisionShapeAssetBuilder.
    class CollisionShapeSource : public ISerializable
    {
        DRACONIC_OBJECT(CollisionShapeSource, ISerializable)
    public:
        bool convex = false; // informational (the blob self-describes)
        Array<u8> shapeBlob; // Jolt binary shape state
        Array<f32> outline;  // debug triangles: x,y,z per vertex, 9 floats per tri

        void Serialize(ISerializer& ar) override
        {
            draconic::foundation::Serialize(ar, "convex", convex);
            draconic::foundation::Serialize(ar, "shapeBlob", shapeBlob);
            draconic::foundation::Serialize(ar, "outline", outline);
        }
    };

    // Runtime product: what RigidBody/Collider components bind via Ref<CollisionShape>.
    class CollisionShape : public Object
    {
        DRACONIC_OBJECT(CollisionShape, Object)
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
        [[nodiscard]] RefPtr<Object> Create(resource::ResourceManager&,
                                            draconic::content::Instance& instance) override
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
        DRACONIC_OBJECT(PhysicalMaterialSource, ISerializable)
    public:
        f32 friction = 0.5f;
        f32 restitution = 0.0f;
        f32 density = 1000.0f; // kg/m^3

        void Serialize(ISerializer& ar) override
        {
            draconic::foundation::Serialize(ar, "friction", friction);
            draconic::foundation::Serialize(ar, "restitution", restitution);
            draconic::foundation::Serialize(ar, "density", density);
        }
    };

    class PhysicalMaterial : public Object
    {
        DRACONIC_OBJECT(PhysicalMaterial, Object)
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
        [[nodiscard]] RefPtr<Object> Create(resource::ResourceManager&,
                                            draconic::content::Instance& instance) override
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

    // Registers the cooked record + product types (content-DB construction by type name).
    inline void RegisterPhysicsResource()
    {
        GlobalTypeRegistry().Register(CollisionShapeSource::StaticType());
        RegisterSerializable<CollisionShapeSource>();
        GlobalTypeRegistry().Register(CollisionShape::StaticType());
        GlobalTypeRegistry().Register(PhysicalMaterialSource::StaticType());
        RegisterSerializable<PhysicalMaterialSource>();
        GlobalTypeRegistry().Register(PhysicalMaterial::StaticType());
    }

    DRACONIC_DEFINE_OBJECT(CollisionShapeSource, "draconic::physics")
    DRACONIC_DEFINE_OBJECT(CollisionShape, "draconic::physics")
    DRACONIC_DEFINE_OBJECT(PhysicalMaterialSource, "draconic::physics")
    DRACONIC_DEFINE_OBJECT(PhysicalMaterial, "draconic::physics")
}
