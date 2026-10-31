// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Pipeline::Physics - the `foundation.physics.editor` module (tooling).
//
// Source-side physics authoring and cook:
//   * CollisionShapeAsset (pipeline::Asset): references a source mesh asset by guid, with
//     cook settings (convex or triangle mesh, hull tolerance). The builder cooks through Jolt
//     from the mesh's already-extracted StaticMeshSource (positions, indices, material slots)
//     with no glTF or FBX reload, and declares a hash-chained `reads` edge on the mesh, so a
//     model reimport re-cooks the shape.
//   * PhysicalMaterialAsset: plain authored surface properties, cooked to a PhysicalMaterialSource.
//
// Never linked by the runtime.

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"
#include "Core/Reflection/Reflect.h"

export module physics.pipeline;

import foundation.core;
import pipeline.core;
import foundation.content;
import foundation.geometry;
import geometry.pipeline;
import foundation.geometry.resource;
import foundation.physics;
import foundation.physics.resource;

using namespace foundation::core;
using namespace foundation::physics;

export namespace pipeline{
    enum class CollisionCookKind : u8
    {
        ConvexHull = 0, // dynamic-capable simplified hull
        TriangleMesh,   // exact static geometry (per-face material slots)
    };

    // Source asset: which mesh to cook, and how.
    class CollisionShapeAsset final : public pipeline::Asset
    {
        RTTI_OBJECT(CollisionShapeAsset, pipeline::Asset)
    public:
        Guid sourceMesh; // StaticMeshAsset guid
        CollisionCookKind cook = CollisionCookKind::ConvexHull;
        f32 hullTolerance = 1.0e-3f; // convex: simplification slack

        void Serialize(ISerializer& ar) override
        {
            pipeline::Asset::Serialize(ar); // fileName (unused; guid-sourced)
            foundation::core::Serialize(ar, "sourceMesh", sourceMesh);
            u8 kind = static_cast<u8>(cook);
            foundation::core::Serialize(ar, "cook", kind);
            cook = static_cast<CollisionCookKind>(kind);
            foundation::core::Serialize(ar, "hullTolerance", hullTolerance);
        }
    };

    class CollisionShapeAssetBuilder final : public pipeline::DefaultAssetBuilder
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

        void ScanDependencies(const pipeline::Asset& asset,
                              pipeline::AssetBuildContext&,
                              pipeline::AssetDependencies& out) override
        {
            const CollisionShapeAsset& ca = static_cast<const CollisionShapeAsset&>(asset);
            if (!ca.sourceMesh.IsNil())
            {
                out.reads.PushBack(ca.sourceMesh); // hash-chained: a mesh reimport re-cooks
            }
        }

        [[nodiscard]] Status Build(const pipeline::Asset& asset,
                                   pipeline::AssetBuildContext& ctx) override
        {
            const CollisionShapeAsset& ca = static_cast<const CollisionShapeAsset&>(asset);
            if (ctx.output == nullptr || ctx.db == nullptr)
            {
                return Status{ErrorCode::InvalidArgument};
            }

            foundation::content::Instance* meshInstance = ctx.db->GetInstance(ca.sourceMesh);
            if (meshInstance == nullptr)
            {
                LOG_ERROR(u8"Physics", u8"collision shape: source mesh not found in db");
                return Status{ErrorCode::NotFound};
            }
            RefPtr<ISerializable> object = meshInstance->ReadObject();
            // A `reads` edge (see ScanDependencies) resolves sourceMesh to the mesh's cooked product,
            // a StaticMeshSource, which carries exactly the geometry a shape is cooked from, so that
            // is what the editor cook hands us here. Headless and source-db paths instead yield the
            // StaticMeshAsset that embeds the same source; accept either so the cook works from
            // whichever the db resolves the guid to.
            const foundation::geometry::StaticMeshSource* meshSource = nullptr;
            if (auto* product = Cast<foundation::geometry::StaticMeshSource>(object.Get()))
            {
                meshSource = product;
            }
            else if (auto* meshAsset = Cast<pipeline::StaticMeshAsset>(object.Get()))
            {
                // Mesh envelopes carry no inline geometry, so pull the sidecar stream first.
                const Status loaded = pipeline::EnsureMeshSourceLoaded(*meshInstance, *meshAsset);
                if (!loaded.IsOk())
                {
                    return loaded;
                }
                meshSource = &meshAsset->source;
            }
            if (meshSource == nullptr)
            {
                utf8char guidChars[37];
                ca.sourceMesh.ToChars(guidChars);
                if (object.Get() == nullptr)
                {
                    // The instance header names a type but ReadObject built nothing: the mesh
                    // itself did not deserialize (a stale on-disk schema is the usual cause, the same
                    // "failed to deserialize" the cook logs for other assets). The collision fails
                    // because of the mesh, not on its own account.
                    LOG_ERROR(
                        u8"Physics",
                        u8"collision cook: source mesh '{}' [{}] ({}) did not deserialize - stale "
                        u8"schema? delete + re-import the model, then it recooks fresh",
                        meshInstance->Name(), meshInstance->TypeName(),
                        StringView(guidChars, 36));
                }
                else
                {
                    // Resolved to a real object that is neither a mesh product nor a mesh asset: the
                    // sourceMesh guid points at the wrong instance (a skinned mesh, which has
                    // no collision) or an import-wiring bug.
                    LOG_ERROR(
                        u8"Physics",
                        u8"collision cook: source '{}' [{}] ({}) is neither a mesh product nor a "
                        u8"mesh asset - skinned meshes have no collision; re-import to regenerate",
                        meshInstance->Name(), meshInstance->TypeName(),
                        StringView(guidChars, 36));
                }
                return Status{ErrorCode::InvalidArgument};
            }

            CollisionShapeSource cooked;
            const Status status = CookFromMeshSource(*meshSource, ca, cooked);
            if (!status.IsOk())
            {
                return status;
            }
            return ctx.output->WriteObject(cooked);
        }

        // Shared with the model importer's generate-collision path (cooks without a db).
        [[nodiscard]] static Status
        CookFromMeshSource(const foundation::geometry::StaticMeshSource& mesh,
                           const CollisionShapeAsset& settings, CollisionShapeSource& out)
        {
            const usize stride = sizeof(foundation::geometry::StaticMeshVertex);
            const usize vertexCount = mesh.vertexBlob.Size() / stride;
            if (vertexCount == 0)
            {
                LOG_ERROR(
                    u8"Physics",
                    u8"collision cook: source mesh has no vertices (nothing to cook a shape from)");
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
                    const auto primitive = static_cast<foundation::geometry::PrimitiveType>(
                        s < mesh.subPrim.Size() ? mesh.subPrim[s] : 0);
                    if (primitive != foundation::geometry::PrimitiveType::Triangles)
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
                LOG_ERROR(u8"Physics", u8"collision cook failed ({} vertices)",
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

    // Authored surface properties, cooked to a PhysicalMaterialSource (a straight copy).
    class PhysicalMaterialAsset final : public pipeline::Asset
    {
        RTTI_OBJECT(PhysicalMaterialAsset, pipeline::Asset)
    public:
        f32 friction = 0.5f;
        f32 restitution = 0.0f;
        f32 density = 1000.0f;

        void Serialize(ISerializer& ar) override
        {
            pipeline::Asset::Serialize(ar);
            foundation::core::Serialize(ar, "friction", friction);
            foundation::core::Serialize(ar, "restitution", restitution);
            foundation::core::Serialize(ar, "density", density);
        }
    };

    class PhysicalMaterialAssetBuilder final : public pipeline::DefaultAssetBuilder
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

        [[nodiscard]] Status Build(const pipeline::Asset& asset,
                                   pipeline::AssetBuildContext& ctx) override
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

    // Registers the CollisionCookKind enum reflection (idempotent). The asset type reflection
    // bodies are their StaticType(), defined in PhysicsAssetImpl.cpp.
    void RegisterPhysicsAssetReflection();

    // Registers the asset types for content-DB construction and deserialization.
    inline void RegisterPhysicsAssets()
    {
        RegisterPhysicsAssetReflection(); // CollisionCookKind names for the property grid
        GlobalTypeRegistry().Register(CollisionShapeAsset::StaticType(), TypeDomain(u8"Pipeline"));
        RegisterSerializable<CollisionShapeAsset>();
        GlobalTypeRegistry().Register(PhysicalMaterialAsset::StaticType(), TypeDomain(u8"Pipeline"));
        RegisterSerializable<PhysicalMaterialAsset>();
    }

    /// File > New's physics creators (pipeline.registration composes every domain's).
    void RegisterPhysicsCreators(AssetCreatorRegistry& registry);
}
