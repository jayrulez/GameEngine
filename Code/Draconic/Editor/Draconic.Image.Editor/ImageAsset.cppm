// Draconic::ImageEditor - the `draconic.image.editor` module.
//
// Tooling: the source ImageAsset (an image file + color-space intent) and the
// builder that cooks it into a runtime ImageResource (decode the file, write the
// header + "pixels" stream into the output DB). Never linked by the runtime.

module;
#include "Draconic.Core/Prelude.h"
#include "Draconic.Core/Reflection/Reflect.h"

export module draconic.image.editor;

import draconic.core;
import draconic.editor;
import draconic.image;
import draconic.image.io;
import draconic.image.resource;
import draconic.content;

using namespace draconic::core;

export namespace draconic::image
{
    // Source asset: references an image file; colorSpace says how to interpret it.
    class ImageAsset final : public draconic::editor::Asset
    {
        DRACONIC_OBJECT(ImageAsset, draconic::editor::Asset)
    public:
        ImageColorSpace colorSpace = ImageColorSpace::Srgb;

        void Serialize(ISerializer& ar) override
        {
            draconic::editor::Asset::Serialize(ar); // fileName
            draconic::core::Serialize(ar, "colorSpace", colorSpace);
        }
    };

    // Cooks an ImageAsset -> ImageResource (decode file -> header + pixel stream).
    class ImageAssetBuilder final : public draconic::editor::DefaultAssetBuilder
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

        [[nodiscard]] Status Build(const draconic::editor::Asset& asset,
                                   draconic::editor::AssetBuildContext& ctx) override
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

    // Registers ImageAsset for content-DB construction + deserialization. ImageAsset's own
    // reflection body (colorSpace property) is its StaticType(), in ImageAssetImpl.cpp; the
    // ImageColorSpace enum reflection lives in draconic.image. Reflection track P1.
    inline void RegisterImageAsset()
    {
        RegisterImageReflection(); // ImageColorSpace names for the property grid
        GlobalTypeRegistry().Register(ImageAsset::StaticType(), TypeDomain(u8"Editor"));
        RegisterSerializable<ImageAsset>();
    }
}
