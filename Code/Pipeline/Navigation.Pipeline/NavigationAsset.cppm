// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Pipeline::Navigation - the `navigation.pipeline` module (tooling).
//
// Source-side navmesh authoring and cook:
//   * NavigationZoneAsset (pipeline::Asset): carries the baked navmesh blob the editor's "Bake
//     Navigation" action produced. The blob is machine-generated bulk, so it travels in a
//     sidecar stream rather than inline in the text envelope, as all bulk data does.
//   * NavigationZoneAssetBuilder: cooks the blob through to a NavigationZoneSource product. It
//     is a pure passthrough (the bake already ran in the editor, and the cook has no scene
//     geometry), so this module never touches Recast or Detour.
//
// There is no OS-file importer: zones are authored in the scene (a NavMeshZoneComponent), never
// dropped as files. Never linked by the runtime.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module navigation.pipeline;

import foundation.core;
import pipeline.core;
import foundation.content;
import foundation.navigation.resource;

using namespace foundation::core;

export namespace pipeline
{
    // The data stream carrying the baked navmesh beside the asset envelope.
    inline constexpr StringView kNavMeshStreamName = u8"navmesh";

    // Source asset: the baked navmesh for one zone. `navMeshBlob` is filled from the sidecar on
    // load (WriteNavigationZoneAsset, EnsureNavMeshLoaded); it is not serialized inline.
    class NavigationZoneAsset final : public pipeline::Asset
    {
        RTTI_OBJECT(NavigationZoneAsset, pipeline::Asset)
    public:
        Array<u8> navMeshBlob; // baked navmesh (header and Detour tiles); travels in the sidecar

        void Serialize(ISerializer& ar) override
        {
            pipeline::Asset::Serialize(ar); // fileName (unused; zones are authored in the scene)
            // navMeshBlob is not serialized here: it is bulk, so it lives in the `navmesh`
            // sidecar stream, never inline in this (possibly text) envelope.
        }
    };

    // Save an asset and its baked navmesh sidecar together. Every save path (the editor bake action)
    // uses this so the blob and envelope stay in sync.
    [[nodiscard]] inline Status WriteNavigationZoneAsset(foundation::content::Instance& instance,
                                                         NavigationZoneAsset& asset)
    {
        const Status envelope = instance.WriteObject(asset);
        if (!envelope.IsOk())
        {
            return envelope;
        }
        return instance.WriteData(kNavMeshStreamName, Span<const byte>{
                                                          reinterpret_cast<const byte*>(
                                                              asset.navMeshBlob.Data()),
                                                          asset.navMeshBlob.Size()});
    }

    // After ReadObject: pull the sidecar into `asset.navMeshBlob`. A missing stream is an unbaked
    // zone (empty blob), valid and not an error; it cooks to an invalid navmesh the subsystem skips.
    [[nodiscard]] inline Status EnsureNavMeshLoaded(const foundation::content::Instance& instance,
                                                    NavigationZoneAsset& asset)
    {
        asset.navMeshBlob.Clear();
        UniquePtr<IStream> stream = instance.ReadData(kNavMeshStreamName);
        if (!stream)
        {
            return Status{}; // unbaked zone: no sidecar yet
        }
        const i64 size = stream->Size();
        if (size <= 0)
        {
            return Status{};
        }
        asset.navMeshBlob.Resize(static_cast<usize>(size));
        const u64 read = stream->Read(asset.navMeshBlob.Data(), asset.navMeshBlob.Size());
        if (read != asset.navMeshBlob.Size())
        {
            asset.navMeshBlob.Clear();
            return Status{ErrorCode::Internal}; // truncated sidecar
        }
        return Status{};
    }

    class NavigationZoneAssetBuilder final : public pipeline::DefaultAssetBuilder
    {
    public:
        [[nodiscard]] const TypeInfo* AssetType() const override
        {
            return &NavigationZoneAsset::StaticType();
        }
        [[nodiscard]] const TypeInfo* ProductType() const override
        {
            return &foundation::navigation::NavigationZoneSource::StaticType();
        }
        // 3: the product carries no frame-convention stamp (every bake is rigid-frame).
        [[nodiscard]] u32 Version() const override { return 3; }

        void ScanDependencies(const pipeline::Asset&, pipeline::AssetBuildContext&,
                              pipeline::AssetDependencies& out) override
        {
            // The baked navmesh sidecar is the build input: declare it so the recipe hash covers
            // it (a rebake must dirty the cook, and the envelope carries nothing).
            out.sourceStreams.PushBack(String(kNavMeshStreamName));
        }

        [[nodiscard]] Status Build(const pipeline::Asset& asset,
                                   pipeline::AssetBuildContext& ctx) override
        {
            NavigationZoneAsset& na =
                const_cast<NavigationZoneAsset&>(static_cast<const NavigationZoneAsset&>(asset));
            if (ctx.output == nullptr)
            {
                return Status{ErrorCode::InvalidArgument};
            }
            if (ctx.source != nullptr)
            {
                const Status loaded = EnsureNavMeshLoaded(*ctx.source, na);
                if (!loaded.IsOk())
                {
                    return loaded;
                }
            }
            foundation::navigation::NavigationZoneSource product;
            product.navMeshBlob = na.navMeshBlob;
            return ctx.output->WriteObject(product);
        }
    };

    // Registers the source asset type and serializer (content-DB construction by type name).
    inline void RegisterNavigationZoneAsset()
    {
        GlobalTypeRegistry().Register(NavigationZoneAsset::StaticType());
        RegisterSerializable<NavigationZoneAsset>();
    }


    /// File > New's navigation creators (pipeline.registration composes every domain's).
    void RegisterNavigationCreators(AssetCreatorRegistry& registry);
}
