// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Pipeline::Geometry - the `foundation.geometry.editor` module (tooling).
//
// Source-side mesh authoring and cook:
//   * StaticMeshAsset and SkinnedMeshAsset (pipeline::Asset): wrap a resolved
//     StaticMeshSource or SkinnedMeshSource. The model importer fills them from an
//     imported model.
//   * The asset builders write the resolved source into the product DB, where the
//     mesh factories build it into a StaticMesh or SkinnedMesh.
//   * MeshImporter: capture a StaticMesh or SkinnedMesh built in code (a primitive,
//     say) into an asset.
//
// Never linked by the runtime.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module geometry.pipeline;

import foundation.core;
import pipeline.core;
import foundation.content;
import foundation.geometry;
import foundation.geometry.resource;

using namespace foundation::core;
using namespace foundation::geometry;

export namespace pipeline{

    // The mesh bulk sidecar: geometry lives in a binary data stream beside the envelope,
    // not inline in the XML. Vertex arrays as text make a large model's envelope hundreds of
    // megabytes, and every project open would DOM-parse it to read a few header fields.
    // Sources are text for authored data; machine-generated bulk goes to a binary sidecar, as
    // texture pixels do, written by the BinarySerializer under the mesh source type's version
    // scope (the sidecar is stamped and checked like an envelope).
    inline constexpr StringView kMeshGeometryStreamName = u8"geometry";

    class StaticMeshAsset final : public pipeline::Asset
    {
        RTTI_OBJECT(StaticMeshAsset, pipeline::Asset)
    public:
        // `source` always travels in the kMeshGeometryStreamName sidecar; the envelope holds
        // only fileName. Readers call EnsureMeshSourceLoaded after ReadObject.
        StaticMeshSource source;

        void Serialize(ISerializer& ar) override
        {
            pipeline::Asset::Serialize(ar); // fileName (source model note)
        }
    };

    class SkinnedMeshAsset final : public pipeline::Asset
    {
        RTTI_OBJECT(SkinnedMeshAsset, pipeline::Asset)
    public:
        SkinnedMeshSource source; // sidecar-only, see StaticMeshAsset

        void Serialize(ISerializer& ar) override { pipeline::Asset::Serialize(ar); }
    };

    // === Sidecar reading and writing (shared by the importer, the builders, and every source reader) ===

    namespace detail
    {
        // Serialize `source` into sidecar bytes under the source type's version scope, the
        // same scope MeshSourceFromStream reads under (a sidecar stamped with another version
        // is refused, like an envelope).
        template <typename Source>
        inline void MeshSourceToBytes(Source& source, Array<byte>& out)
        {
            MemoryStream buffer;
            BinarySerializer ar(buffer, SerializeMode::Write);
            BeginVersionedPayload(ar, Source::StaticType()); // the source type stamps its layout
            source.Serialize(ar);
            EndVersionedPayload(ar);
            const Span<const byte> bytes = buffer.Bytes();
            out.Resize(bytes.Size());
            if (bytes.Size() != 0)
            {
                MemCopy(out.Data(), bytes.Data(), bytes.Size());
            }
        }

        template <typename Source>
        [[nodiscard]] inline Status MeshSourceFromStream(IStream& stream, Source& source)
        {
            BinarySerializer ar(stream, SerializeMode::Read);
            BeginVersionedPayload(ar, Source::StaticType()); // read mode: pushes the stored chain
            source.Serialize(ar);
            EndVersionedPayload(ar);
            return ar.IsOk() ? Status{} : Status{ErrorCode::InvalidArgument};
        }
    }

    /// Write `asset` with a small envelope and a binary geometry stream. The one writer
    /// every save path uses (importer, primitive capture, page save).
    template <typename Asset>
    [[nodiscard]] inline Status WriteMeshAsset(foundation::content::Instance& instance,
                                               Asset& asset)
    {
        const Status envelope = instance.WriteObject(asset);
        if (!envelope.IsOk())
        {
            return envelope;
        }
        Array<byte> bytes;
        detail::MeshSourceToBytes(asset.source, bytes);
        return instance.WriteData(kMeshGeometryStreamName,
                                  Span<const byte>(bytes.Data(), bytes.Size()));
    }

    /// After ReadObject: pull the sidecar into `asset.source` (the envelope never carries it).
    template <typename Asset>
    [[nodiscard]] inline Status EnsureMeshSourceLoaded(const foundation::content::Instance& instance,
                                                       Asset& asset)
    {
        UniquePtr<IStream> stream = instance.ReadData(kMeshGeometryStreamName);
        if (!stream)
        {
            return Status{ErrorCode::NotFound}; // a missing sidecar is a broken asset
        }
        return detail::MeshSourceFromStream(*stream, asset.source);
    }

    /// Cook-time mesh optimization stats, logged by the builder and
    /// asserted by the cook tests.
    struct MeshOptimizeStats
    {
        u32 triangleSubmeshes = 0; // submeshes the reorder passes ran on
        u32 verticesBefore = 0;
        u32 verticesAfter = 0;     // < before when unused vertices were compacted away
        f32 acmrBefore = 0.0f;     // average cache miss ratio over all triangle ranges
        f32 acmrAfter = 0.0f;
    };

    /// Auto-LOD generation. Ratios are the ladder {0.5, 0.25, 0.125}; a level is dropped
    /// (and the chain ends) when simplification cannot get near its target within the error
    /// bound, so a chain is as long as quality allows and never padded. minTriangles floors
    /// the chain, since simplifying tiny meshes further gains nothing. Thresholds use the
    /// same halving coverage ladder authored chains get.
    struct LodGenerationSettings
    {
        f32 targetError = 0.02f; // meshopt relative error bound (fraction of mesh extent)
        u32 minTriangles = 64;   // stop once a level would fall below this
    };

    /// Generate a LOD chain into `source` via meshopt_simplify (border-locked per submesh
    /// so cross-submesh seams never crack). No-op (returns 0 levels added) when the source
    /// already has a chain (authored wins), has no triangle submeshes, or is malformed.
    /// Deterministic. Defined in MeshOptimizeImpl.cpp.
    u32 GenerateLodChain(StaticMeshSource& source, const LodGenerationSettings& settings = {});

    /// Vertex-cache and overdraw reorder per triangle submesh, then one
    /// whole-mesh vertex-fetch remap (reorders the blob, rewrites every index, compacts
    /// unused vertices). The triangle set, submesh ranges and vertex values are preserved;
    /// only order changes, so rendering is identical. Non-triangle submeshes keep their
    /// index order (the remap still rewrites their index values). Deterministic
    /// (meshoptimizer has no threading or RNG). Skinned sources are included: the parallel
    /// skinning stream receives the identical vertex permutation (a size-mismatched
    /// stream refuses the whole pass).
    /// Defined in MeshOptimizeImpl.cpp (meshoptimizer stays out of this interface).
    void OptimizeStaticMeshSource(StaticMeshSource& source, MeshOptimizeStats* outStats = nullptr);

    // Cooks a StaticMeshAsset into a StaticMeshSource in the output DB.
    class StaticMeshAssetBuilder final : public pipeline::DefaultAssetBuilder
    {
    public:
        void ScanDependencies(const pipeline::Asset& asset,
                              pipeline::AssetBuildContext&,
                              pipeline::AssetDependencies& out) override
        {
            // The geometry sidecar is a build input: declare it so the recipe hash covers it
            // (a geometry-only change must dirty the cook, and the envelope does not carry it).
            (void)asset;
            out.sourceStreams.PushBack(String(kMeshGeometryStreamName));
        }

        // Cook version 2: StaticMeshSource 5 (Float4 tangent, LOD chain, geometry never inline); a product cooked under the older record
        // version is refused by the reader, so the recipe hash must change to re-cook it.
        [[nodiscard]] u32 Version() const override { return 2; }

        [[nodiscard]] const TypeInfo* AssetType() const override
        {
            return &StaticMeshAsset::StaticType();
        }
        [[nodiscard]] const TypeInfo* ProductType() const override
        {
            return &StaticMeshSource::StaticType();
        }
        [[nodiscard]] Status Build(const pipeline::Asset& asset,
                                   pipeline::AssetBuildContext& ctx) override
        {
            StaticMeshAsset& ma = const_cast<StaticMeshAsset&>(
                static_cast<const StaticMeshAsset&>(asset)); // sidecar load fills `source`
            if (ctx.output == nullptr)
            {
                return Status{ErrorCode::InvalidArgument};
            }
            if (ctx.source != nullptr)
            {
                const Status loaded = EnsureMeshSourceLoaded(*ctx.source, ma);
                if (!loaded.IsOk())
                {
                    return loaded;
                }
            }
            // Optimization pass: same triangles, better order, unused vertices compacted.
            // In place on the per-cook source instance; idempotent on re-cooks.
            OptimizeStaticMeshSource(ma.source);
            return ctx.output->WriteObject(ma.source);
        }
    };

    // Cooks a SkinnedMeshAsset into a SkinnedMeshSource in the output DB.
    class SkinnedMeshAssetBuilder final : public pipeline::DefaultAssetBuilder
    {
    public:
        void ScanDependencies(const pipeline::Asset& asset,
                              pipeline::AssetBuildContext&,
                              pipeline::AssetDependencies& out) override
        {
            (void)asset;
            out.sourceStreams.PushBack(String(kMeshGeometryStreamName));
        }

        // Cook version 2: the mesh source layout, in step with the static builder; a product cooked under the older record
        // version is refused by the reader, so the recipe hash must change to re-cook it.
        [[nodiscard]] u32 Version() const override { return 2; }

        [[nodiscard]] const TypeInfo* AssetType() const override
        {
            return &SkinnedMeshAsset::StaticType();
        }
        [[nodiscard]] const TypeInfo* ProductType() const override
        {
            return &SkinnedMeshSource::StaticType();
        }
        [[nodiscard]] Status Build(const pipeline::Asset& asset,
                                   pipeline::AssetBuildContext& ctx) override
        {
            SkinnedMeshAsset& ma = const_cast<SkinnedMeshAsset&>(
                static_cast<const SkinnedMeshAsset&>(asset));
            if (ctx.output == nullptr)
            {
                return Status{ErrorCode::InvalidArgument};
            }
            if (ctx.source != nullptr)
            {
                const Status loaded = EnsureMeshSourceLoaded(*ctx.source, ma);
                if (!loaded.IsOk())
                {
                    return loaded;
                }
            }
            // Skinned included: the parallel skinning stream is permuted with
            // the identical remap (see MeshOptimizeImpl) so the streams cannot diverge.
            OptimizeStaticMeshSource(ma.source);
            return ctx.output->WriteObject(ma.source);
        }
    };

    // Authoring helpers: capture a mesh built in code into an asset.
    class MeshImporter
    {
    public:
        static void Import(const StaticMesh& mesh, StaticMeshAsset& outAsset)
        {
            StaticMeshSource::FromMesh(mesh, outAsset.source);
        }
        static void Import(const SkinnedMesh& mesh, SkinnedMeshAsset& outAsset)
        {
            SkinnedMeshSource::FromMesh(mesh, outAsset.source);
        }
    };

    inline void RegisterMeshAssets()
    {
        GlobalTypeRegistry().Register(StaticMeshAsset::StaticType(), TypeDomain(u8"Pipeline"));
        RegisterSerializable<StaticMeshAsset>();
        GlobalTypeRegistry().Register(SkinnedMeshAsset::StaticType(), TypeDomain(u8"Pipeline"));
        RegisterSerializable<SkinnedMeshAsset>();
    }

    // The mesh assets' data version (5, in GeometryCreatorsImpl.cpp) keeps geometry in the
    // sidecar only. It moves in step with StaticMeshSource (MeshResource.cppm), whose layout
    // the sidecar carries under this asset's version scope.


    /// A static mesh asset of `mesh` in `target`, named `baseName` made unique, its geometry in
    /// the binary sidecar (WriteMeshAsset). The primitive creators and the new-project seed
    /// make meshes through this.
    [[nodiscard]] foundation::content::Instance* CreatePrimitiveMeshInstance(
        foundation::content::Group* target, StringView baseName,
        RefPtr<foundation::geometry::StaticMesh> mesh);
    /// File > New's primitive creators (pipeline.registration composes every domain's).
    void RegisterGeometryCreators(AssetCreatorRegistry& registry);
} // namespace foundation::geometry
