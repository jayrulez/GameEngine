// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Pipeline::Image - the `foundation.image.editor` module.
//
// Tooling: the source ImageAsset (an image file and its color space) and the
// builder that cooks it into a runtime ImageResource (decode the file, write the
// header and "pixels" stream into the output DB). Never linked by the runtime.

module;
#include <initializer_list>
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module image.pipeline;

import foundation.core;
import foundation.vfs; // SourcePath
import pipeline.core;
import pipeline.importer; // IFileImporter, ImportContext, CopyIntoSources
import foundation.image;
import foundation.image.io;
import foundation.image.resource;
import foundation.content;

using namespace foundation::core;
using namespace foundation::image;
namespace content = foundation::content;

export namespace pipeline{
    // Source asset: references an image file; colorSpace says how to interpret it.
    class ImageAsset final : public pipeline::Asset
    {
        RTTI_OBJECT(ImageAsset, pipeline::Asset)
    public:
        ImageColorSpace colorSpace = ImageColorSpace::Srgb;

        void Serialize(ISerializer& ar) override
        {
            pipeline::Asset::Serialize(ar); // fileName
            foundation::core::Serialize(ar, "colorSpace", colorSpace);
        }
    };

    // Cooks an ImageAsset into an ImageResource (decodes the file into a header and pixel stream).
    class ImageAssetBuilder final : public pipeline::DefaultAssetBuilder
    {
    public:
        [[nodiscard]] const TypeInfo* AssetType() const override
        {
            return &ImageAsset::StaticType();
        }
        [[nodiscard]] const TypeInfo* ProductType() const override
        {
            return &ImageResource::StaticType();
        }

        [[nodiscard]] Status Build(const pipeline::Asset& asset,
                                   pipeline::AssetBuildContext& ctx) override
        {
            const ImageAsset& ia = static_cast<const ImageAsset&>(asset); // guarded by AssetType()
            if (ctx.output == nullptr)
            {
                return Status{ErrorCode::InvalidArgument};
            }

            Image image;
            Result<Array<byte>> bytes = ReadSourceBytes(ctx, ia.fileName.View());
            if (!bytes.HasValue())
            {
                return Status{bytes.Error()};
            }
            const Status loaded = io::LoadImageFromMemory(
                Span<const u8>(reinterpret_cast<const u8*>(bytes.Value().Data()),
                               bytes.Value().Size()),
                image);
            if (!loaded.IsOk())
            {
                return loaded;
            }

            ImageResource resource;
            resource.width = image.Width();
            resource.height = image.Height();
            resource.format = image.Format();
            resource.colorSpace = ia.colorSpace;

            const Status wrote = ctx.output->WriteObject(resource);
            if (!wrote.IsOk())
            {
                return wrote;
            }

            const Span<const u8> px = image.PixelData();
            return ctx.output->WriteData(
                u8"pixels", Span<const byte>(reinterpret_cast<const byte*>(px.Data()), px.Size()));
        }
    };

    // OS-file importer (editor drag-drop): imports an image file as an ImageAsset (a raw image and
    // its color space), distinct from a cooked TextureAsset. Claims the same extensions as the
    // texture importer, so dropping a .png offers the importer chooser (Image or Texture).
    class ImageFileImporter final : public pipeline::IFileImporter
    {
    public:
        [[nodiscard]] StringView Label() const override { return u8"Image"; }

        [[nodiscard]] bool Accepts(StringView extension) const override
        {
            for (StringView ext : {u8"png", u8"jpg", u8"jpeg", u8"tga", u8"bmp", u8"hdr"})
            {
                if (extension == ext)
                {
                    return true;
                }
            }
            return false;
        }

        [[nodiscard]] pipeline::ImportPlan DescribeImport(StringView sourcePath,
                                                          const pipeline::ImportOptions*,
                                                          Object*) override
        {
            return pipeline::SingleAssetPlan(sourcePath); // one asset, named after the stem
        }

        [[nodiscard]] StringView AssetTypeName(StringView) const override { return u8"ImageAsset"; }

        [[nodiscard]] pipeline::ImportPlan StoredSelection(content::Group& group,
                                                           StringView sourcePath) override
        {
            return pipeline::SingleAssetStoredSelection(group, sourcePath, u8"ImageAsset");
        }

        [[nodiscard]] Result<content::Instance*>
        Import(StringView sourcePath, const pipeline::ImportContext& context, content::Group& group,
               const pipeline::ImportOptions* options, Object*,
               Array<pipeline::DeferredImportWrite>*) override
        {
            // The name claimed by the clash rule, the file copied in beside any other's (pipeline::ClaimSingleAsset).
            String fileName;
            Result<content::Instance*> claimed = pipeline::ClaimSingleAsset(context, group, options, sourcePath, ImageAsset::StaticType(), fileName);
            if (!claimed.HasValue())
            {
                return Err(claimed.Error());
            }
            content::Instance* instance = claimed.Value();

            ImageAsset asset;
            asset.fileName = foundation::vfs::SourcePath(fileName.AsView());
            // .hdr is a linear (HDR) image; the LDR formats are sRGB color by default.
            asset.colorSpace = (pipeline::FileExtensionLower(sourcePath) == u8"hdr")
                                   ? ImageColorSpace::Linear
                                   : ImageColorSpace::Srgb;
            const Status written = instance->WriteObject(asset);
            if (!written.IsOk())
            {
                return Err(written.Code());
            }
            return instance;
        }
    };

    // Registers ImageAsset for content-DB construction and deserialization. ImageAsset's own
    // reflection body (colorSpace property) is its StaticType(), in ImageAssetImpl.cpp; the
    // ImageColorSpace enum reflection lives in foundation.image.
    inline void RegisterImageAsset()
    {
        RegisterImageReflection(); // ImageColorSpace names for the property grid
        GlobalTypeRegistry().Register(ImageAsset::StaticType(), TypeDomain(u8"Pipeline"));
        RegisterSerializable<ImageAsset>();
    }
}
