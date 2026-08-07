// Draconic::ImageResource - the `draconic.image.resource` module.
//
// Runtime CPU-image resource (model B): the cooked, device-free pixel data a
// runtime consumer (e.g. the VG renderer) loads through the resource manager and
// uploads itself. Loaded from the OUTPUT content DB; never references the source
// ImageAsset or the builder. Header (dims/format/colorspace) is the serialized
// object; pixels are the heavy "pixels" data stream.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.image.resource;

import draconic.foundation;
import draconic.image;
import draconic.content;
import draconic.resource;

using namespace draconic::foundation;
using namespace draconic::resource;

export namespace draconic::image
{
    // Cooked CPU image: dims/format/colorspace + owned pixels. The runtime product.
    class ImageResource final : public ISerializable
    {
        DRACONIC_OBJECT(ImageResource, ISerializable)
    public:
        u32 width = 0;
        u32 height = 0;
        PixelFormat format = PixelFormat::RGBA8;
        ImageColorSpace colorSpace = ImageColorSpace::Srgb;

        void Serialize(ISerializer& ar) override
        {
            draconic::foundation::Serialize(ar, "width", width);
            draconic::foundation::Serialize(ar, "height", height);
            draconic::foundation::Serialize(ar, "format", format);
            draconic::foundation::Serialize(ar, "colorSpace", colorSpace);
        }

        [[nodiscard]] Span<const u8> Pixels() const noexcept
        {
            return Span<const u8>(m_pixels.Data(), m_pixels.Size());
        }
        void SetPixels(Array<u8>&& pixels) noexcept { m_pixels = Move(pixels); }

        // Non-owning ImageData view over the pixels (for IImageData consumers).
        [[nodiscard]] ImageDataRef View() const
        {
            return ImageDataRef(width, height, format, m_pixels.Data(), m_pixels.Size(),
                                colorSpace);
        }

    private:
        Array<u8> m_pixels;
    };

    // Loads a cooked ImageResource (header + "pixels" stream) from the output DB.
    class ImageFactory final : public IResourceFactory
    {
    public:
        [[nodiscard]] const TypeInfo* ProductType() const override
        {
            return &ImageResource::StaticType();
        }

        [[nodiscard]] RefPtr<Object> Create(ResourceManager& manager,
                                            draconic::content::Instance& instance) override
        {
            (void)manager;
            RefPtr<ISerializable> object = instance.ReadObject();
            ImageResource* image = Cast<ImageResource>(object.Get());
            if (image == nullptr)
            {
                return RefPtr<Object>{};
            }

            Array<u8> pixels;
            if (UniquePtr<IStream> stream = instance.ReadData(u8"pixels"))
            {
                const i64 size = stream->Size();
                if (size > 0)
                {
                    pixels.Resize(static_cast<usize>(size));
                    if (stream->Read(pixels.Data(), static_cast<u64>(size)) !=
                        static_cast<u64>(size))
                    {
                        pixels.Clear();
                    }
                }
            }
            image->SetPixels(Move(pixels));
            return object;
        }
    };

    // Registers ImageResource for content-DB construction + deserialization.
    inline void RegisterImageResource()
    {
        GlobalTypeRegistry().Register(ImageResource::StaticType());
        RegisterSerializable<ImageResource>();
    }

    DRACONIC_DEFINE_OBJECT(ImageResource, "draconic::image")
}
