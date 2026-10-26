// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Pipeline::Geometry - the `foundation.geometry.editor` module (tooling).
//
// Source-side mesh authoring + cook:
//   * StaticMeshAsset / SkinnedMeshAsset (pipeline::Asset): wrap a cooked
//     Static/SkinnedMeshSource. (A real pipeline cooks these from an imported model
//     via a ModelMesh->StaticMesh converter - the tooling we did not port; the asset
//     here carries the already-resolved source.)
//   * The asset builders write the resolved source into the product DB (where the
//     mesh factories build it into a Static/SkinnedMesh).
//   * MeshImporter: capture a Static/SkinnedMesh built in code (e.g. a primitive)
//     into an asset.
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

    // The mesh bulk sidecar: geometry lives in a BINARY data stream beside the envelope,
    // not inline in the XML. Incident 2026-08-11: a Sponza import produced a 170 MB XML
    // envelope (vertex arrays as text), and every project open DOM-parsed it to read three
    // header fields - a 25-second freeze and ~5 GB of DOM peak that the allocator never
    // returns. Sources-are-text is for AUTHORED data; machine-generated bulk follows the
    // texture-pixels precedent (binary sidecar), through the BinarySerializer under the
    // mesh SOURCE type's version scope (the sidecar is stamped and checked like an envelope).
    inline constexpr StringView kMeshGeometryStreamName = u8"geometry";

    class StaticMeshAsset final : public pipeline::Asset
    {
        RTTI_OBJECT(StaticMeshAsset, pipeline::Asset)
    public:
        // `source` ALWAYS travels in the kMeshGeometryStreamName sidecar; the envelope holds
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

    // === Sidecar plumbing (shared by the importer, the builders, and every source reader) ===

    namespace detail
    {
        // Serialize `source` into sidecar BYTES under the SOURCE type's version scope - the
        // same scope MeshSourceFromStream reads under (a sidecar stamped with another version
        // is refused, like an envelope).
        template <typename Source>
        inline void MeshSourceToBytes(Source& source, Array<byte>& out)
        {
            MemoryStream buffer;
            BinarySerializer ar(buffer, SerializeMode::Write);
            BeginVersionedPayload(ar, Source::StaticType()); // the SOURCE type stamps its layout
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
            BeginVersionedPayload(ar, Source::StaticType()); // read mode: pushes the STORED chain
            source.Serialize(ar);
            EndVersionedPayload(ar);
            return ar.IsOk() ? Status{} : Status{ErrorCode::InvalidArgument};
        }
    }

    /// Write `asset` sidecar-style: tiny envelope + binary geometry stream. The one writer
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
            return Status{ErrorCode::NotFound}; // sidecar missing = a broken asset, say so
        }
        return detail::MeshSourceFromStream(*stream, asset.source);
    }

    /// Cook-time mesh optimization stats - logged by the builder,
    /// asserted by the cook tests.
    struct MeshOptimizeStats
    {
        u32 triangleSubmeshes = 0; // submeshes the reorder passes ran on
        u32 verticesBefore = 0;
        u32 verticesAfter = 0;     // < before when unused vertices were compacted away
        f32 acmrBefore = 0.0f;     // average cache miss ratio over all triangle ranges
        f32 acmrAfter = 0.0f;
    };

    /// Auto-LOD generation. Ratios are the ladder {0.5, 0.25,
    /// 0.125}; a level is DROPPED (and the chain ends) when simplification cannot get
    /// near its target within the error bound - a chain is as long as quality allows,
    /// never padded. minTriangles floors the chain (no point simplifying tiny meshes
    /// further). Thresholds use the same halving coverage ladder authored chains get.
    struct LodGenerationSettings
    {
        f32 targetError = 0.02f; // meshopt relative error bound (fraction of mesh extent)
        u32 minTriangles = 64;   // stop once a level would fall below this
    };

    /// Generate a LOD chain into `source` via meshopt_simplify (border-locked per submesh
    /// so cross-submesh seams never crack). No-op (returns 0 levels added) when the source
    /// already HAS a chain (authored wins), has no triangle submeshes, or is malformed.
    /// Deterministic. Defined in MeshOptimizeImpl.cpp.
    u32 GenerateLodChain(StaticMeshSource& source, const LodGenerationSettings& settings = {});

    /// Vertex-cache + overdraw reorder per triangle submesh, then one
    /// whole-mesh vertex-fetch remap (reorders the blob, rewrites every index, compacts
    /// unused vertices). Triangle SET, submesh ranges, and vertex VALUES are preserved -
    /// only order changes, so rendering is identical. Non-triangle submeshes keep their
    /// index order (the remap still rewrites their index VALUES). Deterministic
    /// (meshoptimizer has no threading/RNG). Skinned sources included: the parallel
    /// skinning stream receives the identical vertex permutation (a size-mismatched
    /// stream refuses the whole pass).
    /// Defined in MeshOptimizeImpl.cpp (meshoptimizer stays out of this interface).
    void OptimizeStaticMeshSource(StaticMeshSource& source, MeshOptimizeStats* outStats = nullptr);

    // Cooks a StaticMeshAsset -> StaticMeshSource in the output DB.
    class StaticMeshAssetBuilder final : public pipeline::DefaultAssetBuilder
    {
    public:
        void ScanDependencies(const pipeline::Asset& asset,
                              pipeline::AssetBuildContext&,
                              pipeline::AssetDependencies& out) override
        {
            // The geometry sidecar is a build input: declare it so the recipe hash covers it
            // (a geometry-only change must dirty the cook - the envelope does not carry it).
            (void)asset;
            out.sourceStreams.PushBack(String(kMeshGeometryStreamName));
        }

        // Cook version 2 (2026-09-08): StaticMeshSource 5 - Float4 tangent + LOD chain, geometry never inline; a product cooked under the older record
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
            // Optimization pass: same triangles, better order (+ dead-vertex compaction).
            // In place on the per-cook source instance; idempotent on re-cooks.
            OptimizeStaticMeshSource(ma.source);
            return ctx.output->WriteObject(ma.source);
        }
    };

    // Cooks a SkinnedMeshAsset -> SkinnedMeshSource in the output DB.
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

        // Cook version 2 (2026-09-08): the mesh source layout, in lockstep with the static builder; a product cooked under the older record
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

    // 5 = geometry ALWAYS in the sidecar (the inline-geometry flag left the envelope); the
    // number moves in lockstep with StaticMeshSource (MeshResource.cppm), whose layout the
    // sidecar carries under this asset's version scope.


    /// A static mesh asset of `mesh` in `target`, named `baseName` made unique, its geometry in
    /// the binary sidecar (WriteMeshAsset). The primitive creators and the new-project seed
    /// make meshes through this.
    [[nodiscard]] foundation::content::Instance* CreatePrimitiveMeshInstance(
        foundation::content::Group* target, StringView baseName,
        RefPtr<foundation::geometry::StaticMesh> mesh);
    /// File > New's primitive creators (pipeline.registration composes every domain's).
    void RegisterGeometryCreators(AssetCreatorRegistry& registry);
} // namespace foundation::geometry
