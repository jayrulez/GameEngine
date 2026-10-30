// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// vegetation.pipeline - the painted vegetation mask's pipeline: VegetationMaskAsset (the source
// envelope), VegetationMaskAssetBuilder (cook -> VegetationMask resource: metadata object + the
// "densities" stream) and VegetationMaskFileImporter (a PNG -> one density plane per channel).
//
// Two source shapes, like the splatmap: EMBEDDED (created in the editor, painted with the Paint
// Vegetation brush; the planes ride the source instance's "densities" sidecar, written back on
// Save) and IMPORTED (fileName set; the builder decodes the image at cook). A paint over an
// imported mask converts it to embedded (the editable-source convention).

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module vegetation.pipeline;

import foundation.core;
import foundation.vfs;
import pipeline.core;
import pipeline.importer; // IFileImporter + ImportContext + CopyIntoSources
import foundation.image;
import foundation.image.io; // LoadImageFromMemory (RGBA8 decode)
import foundation.vegetation.resource;
import foundation.content;

using namespace foundation::core;
using foundation::vegetation::kVegetationMaskStream;
using foundation::vegetation::VegetationMask;
using foundation::vegetation::VegetationMaskSource;

export namespace pipeline
{
    namespace content = foundation::content;

    class VegetationMaskAsset final : public pipeline::Asset
    {
        RTTI_OBJECT(VegetationMaskAsset, pipeline::Asset)
    public:
        i32 width = 1024; // raster resolution (an authoring choice, independent of the terrain)
        i32 height = 1024;
        u32 planeCount = 1; // one density plane per layer that paints (embedded); 4 for an import

        void Serialize(ISerializer& ar) override
        {
            pipeline::Asset::Serialize(ar); // fileName (import mode; empty = embedded/painted)
            foundation::core::Serialize(ar, "width", width);
            foundation::core::Serialize(ar, "height", height);
            foundation::core::Serialize(ar, "planeCount", planeCount);
        }
    };

    // Cooks a VegetationMaskAsset -> VegetationMask resource (metadata object + the stream).
    class VegetationMaskAssetBuilder final : public pipeline::DefaultAssetBuilder
    {
    public:
        [[nodiscard]] const TypeInfo* AssetType() const override
        {
            return &VegetationMaskAsset::StaticType();
        }
        // Cooked instance type = the SERIALIZED VegetationMaskSource the cook stamps + ReadObject
        // rebuilds (VegetationMaskFactory.ProductType() is the runtime VegetationMask for Bind).
        [[nodiscard]] const TypeInfo* ProductType() const override
        {
            return &VegetationMaskSource::StaticType();
        }
        [[nodiscard]] u32 Version() const override { return 1; }

        // An EMBEDDED (create + paint) asset reads its source stream - declare it so the recipe
        // hash chains its bytes (a paint save must re-cook). An IMPORTED one chains the file,
        // which the base builder already tracks.
        void ScanDependencies(const pipeline::Asset& asset, pipeline::AssetBuildContext&,
                              pipeline::AssetDependencies& out) override
        {
            const VegetationMaskAsset& ma = static_cast<const VegetationMaskAsset&>(asset);
            if (ma.fileName.IsEmpty())
            {
                out.sourceStreams.PushBack(String(kVegetationMaskStream));
            }
        }

        [[nodiscard]] Status Build(const pipeline::Asset& asset,
                                   pipeline::AssetBuildContext& ctx) override
        {
            const VegetationMaskAsset& ma = static_cast<const VegetationMaskAsset&>(asset);
            if (ctx.output == nullptr)
            {
                return Status{ErrorCode::InvalidArgument};
            }
            RefPtr<VegetationMask> mask;
            if (!ma.fileName.View().IsEmpty())
            {
                // IMPORTED: decode as RGBA8 at native size; one density plane per channel.
                Result<Array<byte>> bytes = ReadSourceBytes(ctx, ma.fileName.View());
                if (!bytes.HasValue())
                {
                    return Status{bytes.Error()};
                }
                foundation::image::Image img;
                const Status loaded = foundation::image::io::LoadImageFromMemory(
                    Span<const u8>(reinterpret_cast<const u8*>(bytes.Value().Data()),
                                   bytes.Value().Size()),
                    img);
                if (!loaded.IsOk())
                {
                    return loaded;
                }
                if (img.Format() != foundation::image::PixelFormat::RGBA8)
                {
                    return Status{ErrorCode::NotSupported}; // HDR/other - densities are 8-bit
                }
                mask = MaskFromRgba(img.PixelData(), static_cast<i32>(img.Width()),
                                    static_cast<i32>(img.Height()), *ctx.allocator);
            }
            else
            {
                // EMBEDDED (create + paint): the planes ride the source sidecar. Present = the
                // painted planes; absent = never painted (all-zero: nothing grows, no seeding).
                // A size mismatch is a broken source - the cook fails.
                const i32 w = ma.width > 0 ? ma.width : 1;
                const i32 h = ma.height > 0 ? ma.height : 1;
                const u32 planes =
                    ma.planeCount == 0
                        ? 1u
                        : Min(ma.planeCount, foundation::vegetation::kMaxMaskPlanes);
                const usize expected = static_cast<usize>(w) * static_cast<usize>(h) * planes;
                Array<u8> densities;
                if (ctx.source != nullptr)
                {
                    densities = ReadStream(*ctx.source, kVegetationMaskStream);
                }
                const usize planeBytes = static_cast<usize>(w) * static_cast<usize>(h);
                if (densities.Size() == expected)
                {
                    mask = MakeRef<VegetationMask>(*ctx.allocator, w, h, planes);
                    MemCopy(mask->Densities().Data(), densities.Data(), expected);
                }
                else if (densities.IsEmpty())
                {
                    mask = MakeRef<VegetationMask>(*ctx.allocator, w, h, planes); // all zero
                }
                else if (densities.Size() % planeBytes == 0)
                {
                    // The plane count changed on a painted asset: keep the planes that still
                    // exist (fresh ones start empty; dropped ones are gone) rather than fail.
                    const usize had = densities.Size() / planeBytes;
                    mask = MakeRef<VegetationMask>(*ctx.allocator, w, h, planes);
                    MemCopy(mask->Densities().Data(), densities.Data(),
                            Min(had, static_cast<usize>(planes)) * planeBytes);
                }
                else
                {
                    return Status{ErrorCode::NotSupported}; // a size mismatch (dims changed)
                }
            }
            if (mask.Get() == nullptr || mask->IsEmpty())
            {
                return Status{ErrorCode::InvalidArgument};
            }
            VegetationMaskSource src;
            VegetationMaskSource::FromMask(*mask, src);
            const Status wrote = ctx.output->WriteObject(src);
            if (!wrote.IsOk())
            {
                return wrote;
            }
            return ctx.output->WriteData(kVegetationMaskStream,
                                         VegetationMaskSource::DensityBlob(*mask));
        }

        /// An RGBA8 raster -> a four-plane mask (R, G, B, A = planes 0..3).
        [[nodiscard]] static RefPtr<VegetationMask> MaskFromRgba(Span<const u8> rgba, i32 width,
                                                                 i32 height, IAllocator& allocator)
        {
            if (width <= 0 || height <= 0 ||
                rgba.Size() < static_cast<usize>(width) * static_cast<usize>(height) * 4u)
            {
                return MakeRef<VegetationMask>(allocator);
            }
            RefPtr<VegetationMask> mask = MakeRef<VegetationMask>(allocator, width, height, 4u);
            const usize texels = static_cast<usize>(width) * static_cast<usize>(height);
            for (u32 plane = 0; plane < 4; ++plane)
            {
                Span<u8> dst = mask->Plane(plane);
                for (usize i = 0; i < texels; ++i)
                {
                    dst[i] = rgba[i * 4u + plane];
                }
            }
            return mask;
        }

    private:
        [[nodiscard]] static Array<u8> ReadStream(content::Instance& instance, StringView name)
        {
            Array<u8> blob;
            if (UniquePtr<IStream> stream = instance.ReadData(name))
            {
                const i64 size = stream->Size();
                if (size > 0)
                {
                    blob.Resize(static_cast<usize>(size));
                    if (stream->Read(blob.Data(), static_cast<u64>(size)) !=
                        static_cast<u64>(size))
                    {
                        blob.Clear();
                    }
                }
            }
            return blob;
        }
    };

    // OS-file importer (editor drag-drop / Import): a PNG (or any stb-decodable image) as a
    // VegetationMaskAsset with fileName set - the builder decodes it as one plane per channel.
    class VegetationMaskFileImporter final : public pipeline::IFileImporter
    {
    public:
        [[nodiscard]] StringView Label() const override { return u8"Vegetation Mask"; }
        [[nodiscard]] bool Accepts(StringView extension) const override
        {
            return extension == u8"png";
        }
        [[nodiscard]] pipeline::ImportPlan DescribeImport(StringView sourcePath,
                                                          const pipeline::ImportOptions*,
                                                          Object*) override
        {
            return pipeline::SingleAssetPlan(sourcePath);
        }
        [[nodiscard]] StringView AssetTypeName(StringView) const override { return u8"VegetationMaskAsset"; }

        [[nodiscard]] pipeline::ImportPlan StoredSelection(content::Group& group,
                                                           StringView sourcePath) override
        {
            return pipeline::SingleAssetStoredSelection(group, sourcePath, u8"VegetationMaskAsset");
        }
        [[nodiscard]] Result<content::Instance*>
        Import(StringView sourcePath, const pipeline::ImportContext& context, content::Group& group,
               const pipeline::ImportOptions* options, Object*,
               Array<pipeline::DeferredImportWrite>*) override
        {
            // The name claimed by the clash rule, the file copied in beside any other's (pipeline::ClaimSingleAsset).
            String fileName;
            Result<content::Instance*> claimed = pipeline::ClaimSingleAsset(context, group, options, sourcePath, VegetationMaskAsset::StaticType(), fileName);
            if (!claimed.HasValue())
            {
                return Err(claimed.Error());
            }
            content::Instance* instance = claimed.Value();
            VegetationMaskAsset asset;
            asset.fileName = foundation::vfs::SourcePath(fileName.AsView());
            asset.planeCount = 4; // one plane per channel
            const Status written = instance->WriteObject(asset);
            if (!written.IsOk())
            {
                return Err(written.Code());
            }
            return instance;
        }
    };

    // Registers VegetationMaskAsset for content-DB construction + deserialization (reflection
    // body in VegetationMaskAssetImpl.cpp per GCC module hygiene).
    inline void RegisterVegetationMaskAsset()
    {
        GlobalTypeRegistry().Register(VegetationMaskAsset::StaticType(), TypeDomain(u8"Pipeline"));
        RegisterSerializable<VegetationMaskAsset>();
    }

    /// File > New's vegetation creator (pipeline.registration composes every domain's).
    void RegisterVegetationCreators(AssetCreatorRegistry& registry);
}
