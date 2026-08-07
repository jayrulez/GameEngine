// Draconic::PhysicsEditor - the `draconic.physics.editor` module (tooling).
//
// Source-side physics authoring + cook (docs/design/physics.md §5):
//   * CollisionShapeAsset (editor::Asset): references a source MESH asset by guid +
//     cook settings (convex/trimesh, hull tolerance). The builder cooks via Jolt from
//     the mesh's already-extracted StaticMeshSource (positions/indices/material slots) -
//     no gltf/fbx reload - and declares a hash-chained `reads` edge on the mesh, so a
//     model reimport re-cooks the shape.
//   * PhysicalMaterialAsset: plain authored surface properties -> PhysicalMaterialSource.
//
// Never linked by the runtime.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Log/Log.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.physics.editor;

import draconic.foundation;
import draconic.editor;
import draconic.content;
import draconic.geometry;
import draconic.geometry.editor;
import draconic.geometry.resource;
import draconic.physics;
import draconic.physics.resource;

using namespace draconic::foundation;

export namespace draconic::physics
{
    enum class CollisionCookKind : u8
    {
        ConvexHull = 0, // dynamic-capable simplified hull
        TriangleMesh,   // exact static geometry (per-face material slots)
    };

    // Source asset: which mesh to cook + how.
    class CollisionShapeAsset final : public draconic::editor::Asset
    {
        DRACONIC_OBJECT(CollisionShapeAsset, draconic::editor::Asset)
    public:
        Guid sourceMesh; // StaticMeshAsset guid
        CollisionCookKind cook = CollisionCookKind::ConvexHull;
        f32 hullTolerance = 1.0e-3f; // convex: simplification slack

        void Serialize(ISerializer& ar) override
        {
            draconic::editor::Asset::Serialize(ar); // fileName (unused; guid-sourced)
            draconic::foundation::Serialize(ar, "sourceMesh", sourceMesh);
            u8 kind = static_cast<u8>(cook);
            draconic::foundation::Serialize(ar, "cook", kind);
            cook = static_cast<CollisionCookKind>(kind);
            draconic::foundation::Serialize(ar, "hullTolerance", hullTolerance);
        }
    };

    class CollisionShapeAssetBuilder final : public draconic::editor::DefaultAssetBuilder
    {
    public:
        [[nodiscard]] const TypeInfo* AssetType() const override
        {
            return &CollisionShapeAsset::StaticType();
        }
        [[nodiscard]] const TypeInfo* ProductType() const override
        {
            return &CollisionShapeSource::StaticType();
        }
        [[nodiscard]] u32 Version() const override { return 1; }

        void ScanDependencies(const draconic::editor::Asset& asset,
                              draconic::editor::AssetBuildContext&,
                              draconic::editor::AssetDependencies& out) override
        {
            const CollisionShapeAsset& ca = static_cast<const CollisionShapeAsset&>(asset);
            if (!ca.sourceMesh.IsNil())
            {
                out.reads.PushBack(ca.sourceMesh); // hash-chained: mesh reimport -> recook
            }
        }

        [[nodiscard]] Status Build(const draconic::editor::Asset& asset,
                                   draconic::editor::AssetBuildContext& ctx) override
        {
            const CollisionShapeAsset& ca = static_cast<const CollisionShapeAsset&>(asset);
            if (ctx.output == nullptr || ctx.db == nullptr)
            {
                return Status{ErrorCode::InvalidArgument};
            }

            draconic::content::Instance* meshInstance = ctx.db->GetInstance(ca.sourceMesh);
            if (meshInstance == nullptr)
            {
                DRACONIC_LOG_ERROR(u8"Physics", u8"collision shape: source mesh not found in db");
                return Status{ErrorCode::NotFound};
            }
            RefPtr<ISerializable> object = meshInstance->ReadObject();
            auto* meshAsset = Cast<draconic::geometry::StaticMeshAsset>(object.Get());
            if (meshAsset == nullptr)
            {
                DRACONIC_LOG_ERROR(u8"Physics", u8"collision shape: source is not a mesh asset");
                return Status{ErrorCode::InvalidArgument};
            }

            CollisionShapeSource cooked;
            const Status status = CookFromMeshSource(meshAsset->source, ca, cooked);
            if (!status.IsOk())
            {
                return status;
            }
            return ctx.output->WriteObject(cooked);
        }

        // Shared with the model importer's generate-collision path (cooks without a db).
        [[nodiscard]] static Status
        CookFromMeshSource(const draconic::geometry::StaticMeshSource& mesh,
                           const CollisionShapeAsset& settings, CollisionShapeSource& out)
        {
            const usize stride = sizeof(draconic::geometry::StaticMeshVertex);
            const usize vertexCount = mesh.vertexBlob.Size() / stride;
            if (vertexCount == 0)
            {
                return Status{ErrorCode::InvalidArgument};
            }

            // Positions sit at offset 0 of each vertex.
            Array<Float3> positions;
            positions.Reserve(vertexCount);
            for (usize v = 0; v < vertexCount; ++v)
            {
                Float3 position;
                MemCopy(&position, mesh.vertexBlob.Data() + v * stride, sizeof(Float3));
                positions.PushBack(position);
            }

            Array<byte> blob;
            bool ok = false;
            if (settings.cook == CollisionCookKind::ConvexHull)
            {
                ok = CookConvexHull(Span<const Float3>(positions.Data(), positions.Size()), blob,
                                    settings.hullTolerance);
            }
            else
            {
                // Triangle-list submeshes only; each triangle carries its submesh's
                // material slot (surfaced as RayHit::surface).
                Array<u32> indices;
                Array<u32> slots;
                for (usize s = 0; s < mesh.subStart.Size(); ++s)
                {
                    const auto primitive = static_cast<draconic::geometry::PrimitiveType>(
                        s < mesh.subPrim.Size() ? mesh.subPrim[s] : 0);
                    if (primitive != draconic::geometry::PrimitiveType::Triangles)
                    {
                        continue;
                    }
                    const i32 start = mesh.subStart[s];
                    const i32 count = mesh.subCount[s];
                    const u32 slot =
                        static_cast<u32>(s < mesh.subMaterial.Size() ? mesh.subMaterial[s] : 0);
                    for (i32 i = 0; i + 2 < count; i += 3)
                    {
                        indices.PushBack(mesh.indexData[static_cast<usize>(start + i + 0)]);
                        indices.PushBack(mesh.indexData[static_cast<usize>(start + i + 1)]);
                        indices.PushBack(mesh.indexData[static_cast<usize>(start + i + 2)]);
                        slots.PushBack(slot);
                    }
                }
                ok = CookTriangleMesh(Span<const Float3>(positions.Data(), positions.Size()),
                                      Span<const u32>(indices.Data(), indices.Size()),
                                      Span<const u32>(slots.Data(), slots.Size()), blob);
            }
            if (!ok)
            {
                DRACONIC_LOG_ERROR(u8"Physics", u8"collision cook failed ({} vertices)",
                                   vertexCount);
                return Status{ErrorCode::InvalidArgument};
            }

            out.convex = settings.cook == CollisionCookKind::ConvexHull;
            out.shapeBlob.Clear();
            out.shapeBlob.Resize(blob.Size());
            MemCopy(out.shapeBlob.Data(), blob.Data(), blob.Size());

            Array<Float3> triangles;
            out.outline.Clear();
            if (ExtractShapeTriangles(Span<const byte>(blob.Data(), blob.Size()), triangles))
            {
                out.outline.Reserve(triangles.Size() * 3);
                for (const Float3& v : triangles)
                {
                    out.outline.PushBack(v.x);
                    out.outline.PushBack(v.y);
                    out.outline.PushBack(v.z);
                }
            }
            return Status{};
        }
    };

    // Authored surface properties -> cooked PhysicalMaterialSource (a straight copy).
    class PhysicalMaterialAsset final : public draconic::editor::Asset
    {
        DRACONIC_OBJECT(PhysicalMaterialAsset, draconic::editor::Asset)
    public:
        f32 friction = 0.5f;
        f32 restitution = 0.0f;
        f32 density = 1000.0f;

        void Serialize(ISerializer& ar) override
        {
            draconic::editor::Asset::Serialize(ar);
            draconic::foundation::Serialize(ar, "friction", friction);
            draconic::foundation::Serialize(ar, "restitution", restitution);
            draconic::foundation::Serialize(ar, "density", density);
        }
    };

    class PhysicalMaterialAssetBuilder final : public draconic::editor::DefaultAssetBuilder
    {
    public:
        [[nodiscard]] const TypeInfo* AssetType() const override
        {
            return &PhysicalMaterialAsset::StaticType();
        }
        [[nodiscard]] const TypeInfo* ProductType() const override
        {
            return &PhysicalMaterialSource::StaticType();
        }

        [[nodiscard]] Status Build(const draconic::editor::Asset& asset,
                                   draconic::editor::AssetBuildContext& ctx) override
        {
            const PhysicalMaterialAsset& ma = static_cast<const PhysicalMaterialAsset&>(asset);
            if (ctx.output == nullptr)
            {
                return Status{ErrorCode::InvalidArgument};
            }
            PhysicalMaterialSource cooked;
            cooked.friction = ma.friction;
            cooked.restitution = ma.restitution;
            cooked.density = ma.density;
            return ctx.output->WriteObject(cooked);
        }
    };

    // Registers the CollisionCookKind enum reflection (idempotent). The asset TYPE reflection
    // bodies are their StaticType(), defined in PhysicsAssetImpl.cpp. Reflection track P1.
    void RegisterPhysicsAssetReflection();

    // Registers the asset types for content-DB construction + deserialization.
    inline void RegisterPhysicsAssets()
    {
        RegisterPhysicsAssetReflection(); // CollisionCookKind names for the property grid
        GlobalTypeRegistry().Register(CollisionShapeAsset::StaticType(), TypeDomain(u8"Editor"));
        RegisterSerializable<CollisionShapeAsset>();
        GlobalTypeRegistry().Register(PhysicalMaterialAsset::StaticType(), TypeDomain(u8"Editor"));
        RegisterSerializable<PhysicalMaterialAsset>();
    }
}
