// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Pipeline::Heightfield - the `heightfield.pipeline` module.
//
// Tooling: the source HeightfieldAsset (a target square size, world footprint and Y range,
// optionally backed by a 16-bit heightmap image) and the builder that cooks it into a runtime
// Heightfield (a metadata object and the "heights" sample stream). Heightmap import lives here,
// not in Terrain.Pipeline. Never linked by the runtime.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module heightfield.pipeline;

import foundation.core;
import foundation.vfs; // SourcePath
import pipeline.core;
import pipeline.importer; // IFileImporter, ImportContext, CopyIntoSources
import foundation.image;
import foundation.image.io;
import foundation.heightfield;
import foundation.heightfield.resource;
import foundation.content;

using namespace foundation::core;
namespace content = foundation::content;

export namespace pipeline
{
    using foundation::heightfield::Heightfield;
    using foundation::heightfield::HeightfieldSource;
    using foundation::heightfield::Height;

    // Source asset: a square heightfield (size = 64k+1) over a worldSize XZ footprint, u16 samples
    // mapped onto [minY, maxY]. If the base fileName is set it is a 16-bit heightmap resampled onto
    // the grid; otherwise the grid is blank (flat).
    class HeightfieldAsset final : public pipeline::Asset
    {
        RTTI_OBJECT(HeightfieldAsset, pipeline::Asset)
    public:
        i32 size = 257;                        // square grid side; must be 64k+1
        Float2 worldSize{256.0f, 256.0f};      // XZ footprint (metres)
        f32 minY = 0.0f;                       // world Y at sample 0
        f32 maxY = 64.0f;                      // world Y at sample 65535

        void Serialize(ISerializer& ar) override
        {
            pipeline::Asset::Serialize(ar); // fileName
            foundation::core::Serialize(ar, "size", size);
            foundation::core::Serialize(ar, "worldSize", worldSize);
            foundation::core::Serialize(ar, "minY", minY);
            foundation::core::Serialize(ar, "maxY", maxY);
        }
    };

    // Bilinear-resample a single-channel 16-bit source (srcW x srcH, row-major u16) onto the grid.
    // The source u16 values are the height samples (both quantize onto the asset's [minY, maxY]).
    inline void ResampleHeightmapR16(const Height* src, u32 srcW, u32 srcH, Heightfield& dst)
    {
        if (src == nullptr || srcW == 0 || srcH == 0 || dst.IsEmpty())
        {
            return;
        }
        const i32 s = dst.Size();
        const f32 span = static_cast<f32>(s - 1);
        const f32 su = static_cast<f32>(srcW - 1);
        const f32 sv = static_cast<f32>(srcH - 1);
        const auto sample = [&](u32 x, u32 y) -> f32 {
            const u32 cx = x < srcW ? x : srcW - 1;
            const u32 cy = y < srcH ? y : srcH - 1;
            return static_cast<f32>(src[static_cast<usize>(cy) * srcW + cx]);
        };
        for (i32 gz = 0; gz < s; ++gz)
        {
            const f32 v = (span > 0.0f) ? static_cast<f32>(gz) / span * sv : 0.0f;
            const u32 y0 = static_cast<u32>(v);
            const f32 fy = v - static_cast<f32>(y0);
            for (i32 gx = 0; gx < s; ++gx)
            {
                const f32 u = (span > 0.0f) ? static_cast<f32>(gx) / span * su : 0.0f;
                const u32 x0 = static_cast<u32>(u);
                const f32 fx = u - static_cast<f32>(x0);
                const f32 top = sample(x0, y0) + (sample(x0 + 1, y0) - sample(x0, y0)) * fx;
                const f32 bottom =
                    sample(x0, y0 + 1) + (sample(x0 + 1, y0 + 1) - sample(x0, y0 + 1)) * fx;
                const f32 h = top + (bottom - top) * fy;
                dst.SetSample(gx, gz, static_cast<Height>(h + 0.5f));
            }
        }
    }

    // Cooks a HeightfieldAsset into a Heightfield resource (metadata object and "heights" stream).
    class HeightfieldAssetBuilder final : public pipeline::DefaultAssetBuilder
    {
    public:
        [[nodiscard]] const TypeInfo* AssetType() const override
        {
            return &HeightfieldAsset::StaticType();
        }
        // The cook stamps this as the cooked instance's type, and the runtime ReadObject reconstructs
        // it, so it must be the serialized cooked form (HeightfieldSource), not the runtime product
        // (Heightfield). HeightfieldFactory.ProductType() is the runtime Heightfield that Bind matches;
        // the two differ, as TextureAssetBuilder's and TextureFactory's do.
        [[nodiscard]] const TypeInfo* ProductType() const override
        {
            return &HeightfieldSource::StaticType();
        }
        // v2: the product type is HeightfieldSource. v3: the "holes" stream beside the heights
        // (HeightfieldSource 2); every heightfield re-cooks.
        [[nodiscard]] u32 Version() const override { return 3; }

        // An embedded heightfield (fileName empty: page-created, or converted by a sculpt save)
        // reads the authored "heights" source stream, so declare it and the recipe hash chains its
        // bytes (the envelope hash does not cover sidecars, and a sculpt save must re-cook). An
        // imported one (fileName set) chains the heightmap file, which the base builder tracks.
        void ScanDependencies(const pipeline::Asset& asset, pipeline::AssetBuildContext&,
                              pipeline::AssetDependencies& out) override
        {
            const HeightfieldAsset& ha = static_cast<const HeightfieldAsset&>(asset);
            if (ha.fileName.View().IsEmpty())
            {
                out.sourceStreams.PushBack(String(foundation::heightfield::kHeightStream));
                out.sourceStreams.PushBack(String(foundation::heightfield::kHoleStream));
            }
        }

        [[nodiscard]] Status Build(const pipeline::Asset& asset,
                                   pipeline::AssetBuildContext& ctx) override
        {
            const HeightfieldAsset& ha = static_cast<const HeightfieldAsset&>(asset); // AssetType()-guarded
            if (ctx.output == nullptr)
            {
                return Status{ErrorCode::InvalidArgument};
            }
            // The asset fields should already be valid; snap defensively so a malformed asset
            // still cooks a legal grid rather than failing the runtime's contracts: size onto
            // 64k+1, the XZ footprint away from zero (a zero span NaNs world<->grid math and
            // hands Jolt a zero scale), and the Y range open (maxY > minY, or every sample
            // quantizes to one height and WorldYToSample divides by zero).
            const i32 size = foundation::heightfield::IsValidSize(ha.size)
                                 ? ha.size
                                 : foundation::heightfield::NextValidSize(ha.size);
            constexpr f32 kMinSpan = 0.001f;
            const Float2 worldSize{ha.worldSize.x > kMinSpan ? ha.worldSize.x : kMinSpan,
                                   ha.worldSize.y > kMinSpan ? ha.worldSize.y : kMinSpan};
            const f32 minY = ha.minY;
            const f32 maxY = (ha.maxY > ha.minY + kMinSpan) ? ha.maxY : (ha.minY + kMinSpan);
            RefPtr<Heightfield> hf =
                MakeRef<Heightfield>(*ctx.allocator, size, worldSize, minY, maxY);

            if (!ha.fileName.View().IsEmpty())
            {
                Result<Array<byte>> bytes = ReadSourceBytes(ctx, ha.fileName.View());
                if (!bytes.HasValue())
                {
                    return Status{bytes.Error()};
                }
                foundation::image::Image img;
                const Status loaded = foundation::image::io::LoadImage16FromMemory(
                    Span<const u8>(reinterpret_cast<const u8*>(bytes.Value().Data()),
                                   bytes.Value().Size()),
                    img);
                if (!loaded.IsOk())
                {
                    return loaded;
                }
                ResampleHeightmapR16(reinterpret_cast<const Height*>(img.PixelData().Data()),
                                     img.Width(), img.Height(), *hf);
            }
            else if (ctx.source != nullptr)
            {
                // Embedded: the authored "heights" sidecar is the truth (a sculpt save writes it;
                // fileName empty marks the sidecar authoritative, the editable-source convention
                // splat uses). A missing or mismatched blob leaves the grid flat (a page-created,
                // never-sculpted heightfield).
                if (UniquePtr<IStream> stream =
                        ctx.source->ReadData(foundation::heightfield::kHeightStream))
                {
                    const i64 streamSize = stream->Size();
                    const i64 expected = static_cast<i64>(size) * static_cast<i64>(size) *
                                         static_cast<i64>(sizeof(Height));
                    if (streamSize == expected)
                    {
                        Span<Height> samples = hf->Samples();
                        if (stream->Read(samples.Data(), static_cast<u64>(streamSize)) !=
                            static_cast<u64>(streamSize))
                        {
                            hf = MakeRef<Heightfield>(*ctx.allocator, size, worldSize, minY,
                                                      maxY); // partial read: fall back to flat
                        }
                    }
                }
                // The authored "holes" sidecar, which the hole brush writes. An absent or
                // mismatched one leaves the plane solid.
                if (UniquePtr<IStream> stream =
                        ctx.source->ReadData(foundation::heightfield::kHoleStream))
                {
                    const i64 streamSize = stream->Size();
                    if (streamSize == static_cast<i64>(size) * static_cast<i64>(size))
                    {
                        Array<u8> plane;
                        plane.Resize(static_cast<usize>(streamSize));
                        if (stream->Read(plane.Data(), static_cast<u64>(streamSize)) ==
                            static_cast<u64>(streamSize))
                        {
                            (void)hf->SetHoles(Span<const u8>(plane.Data(), plane.Size()));
                        }
                    }
                }
            }

            HeightfieldSource src;
            HeightfieldSource::FromHeightfield(*hf, src);
            const Status wrote = ctx.output->WriteObject(src);
            if (!wrote.IsOk())
            {
                return wrote;
            }
            const Status heights = ctx.output->WriteData(foundation::heightfield::kHeightStream,
                                                        HeightfieldSource::HeightBlob(*hf));
            if (!heights.IsOk())
            {
                return heights;
            }
            return ctx.output->WriteData(foundation::heightfield::kHoleStream,
                                         HeightfieldSource::HoleBlob(*hf)); // always: one layout
        }
    };

    // OS-file importer (editor drag-drop): imports a 16-bit heightmap as a HeightfieldAsset. Claims
    // .png (offered alongside Image and Texture in the importer chooser) and the raw .r16 extension.
    class HeightfieldFileImporter final : public pipeline::IFileImporter
    {
    public:
        [[nodiscard]] StringView Label() const override { return u8"Heightfield"; }

        [[nodiscard]] bool Accepts(StringView extension) const override
        {
            return extension == u8"png" || extension == u8"r16";
        }

        [[nodiscard]] pipeline::ImportPlan DescribeImport(StringView sourcePath,
                                                          const pipeline::ImportOptions*,
                                                          Object*) override
        {
            return pipeline::SingleAssetPlan(sourcePath); // one asset, named after the stem
        }

        [[nodiscard]] StringView AssetTypeName(StringView) const override { return u8"HeightfieldAsset"; }

        [[nodiscard]] pipeline::ImportPlan StoredSelection(content::Group& group,
                                                           StringView sourcePath) override
        {
            return pipeline::SingleAssetStoredSelection(group, sourcePath, u8"HeightfieldAsset");
        }

        [[nodiscard]] Result<content::Instance*>
        Import(StringView sourcePath, const pipeline::ImportContext& context, content::Group& group,
               const pipeline::ImportOptions* options, Object*,
               Array<pipeline::DeferredImportWrite>*) override
        {
            // The name claimed by the clash rule, the file copied in beside any other's (pipeline::ClaimSingleAsset).
            String fileName;
            Result<content::Instance*> claimed = pipeline::ClaimSingleAsset(context, group, options, sourcePath, HeightfieldAsset::StaticType(), fileName);
            if (!claimed.HasValue())
            {
                return Err(claimed.Error());
            }
            content::Instance* instance = claimed.Value();
            HeightfieldAsset asset;
            asset.fileName = foundation::vfs::SourcePath(fileName.AsView());
            const Status written = instance->WriteObject(asset);
            if (!written.IsOk())
            {
                return Err(written.Code());
            }
            return instance;
        }
    };

    // Registers HeightfieldAsset for content-DB construction and deserialization (the reflection
    // body is in HeightfieldAssetImpl.cpp, out of the interface).
    inline void RegisterHeightfieldAsset()
    {
        GlobalTypeRegistry().Register(HeightfieldAsset::StaticType(), TypeDomain(u8"Pipeline"));
        RegisterSerializable<HeightfieldAsset>();
    }

    /// File > New's heightfield creator (pipeline.registration composes every domain's).
    void RegisterHeightfieldCreators(AssetCreatorRegistry& registry);
}
