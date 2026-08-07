/// Image data types: ImageColorSpace, OwnedImageData, ImageDataRef.
/// Ported from Sedulous.Images.ImageData.

module;
#include "Draconic.Foundation/Prelude.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>

export module draconic.image:image_data;

import draconic.foundation;
import :pixel_format;

using namespace draconic::foundation;

export namespace draconic::image
{

    /// Color space of the pixel data.
    enum class ImageColorSpace : u32
    {
        /// sRGB-encoded (photos, UI). GPU decodes sRGB->linear on sample.
        Srgb,
        /// Linear data (normal maps, masks, HDR). Sampled as-is.
        Linear,
    };

    // Reflects ImageColorSpace for tooling (enum-by-name dropdowns) + scripting. Idempotent;
    // called by consumers that need the enum names (texture/image asset registration). Body
    // lives in the impl unit (gcc module-interface hygiene). Reflection track P1.
    void RegisterImageReflection();

    /// Abstract interface for image data. Implemented by OwnedImageData (owning)
    /// and ImageDataRef (non-owning). Allows renderers and other consumers to
    /// accept either type through a common base.
    class ImageData
    {
    public:
        ImageData() : m_instanceId(NextInstanceId()) {}
        // Copies are new identities; assignment keeps the target's identity (renderer
        // caches key on it - see InstanceId()).
        ImageData(const ImageData&) : m_instanceId(NextInstanceId()) {}
        ImageData& operator=(const ImageData&) { return *this; }
        virtual ~ImageData() = default;
        [[nodiscard]] virtual u32 Width() const = 0;
        [[nodiscard]] virtual u32 Height() const = 0;
        [[nodiscard]] virtual PixelFormat Format() const = 0;
        [[nodiscard]] virtual Span<const u8> PixelData() const = 0;
        [[nodiscard]] virtual ImageColorSpace ColorSpace() const = 0;

        /// Process-unique generation id, minted per constructed instance. Identity-keyed
        /// GPU caches (VGRenderer's texture cache) MUST validate this alongside the raw
        /// pointer: after delete, the allocator can hand a NEW image the SAME address, and
        /// a pointer-only key then serves the dead image's texture (the editor's
        /// rainbow-glyph stale previews). Never key a cache on ImageData* alone.
        [[nodiscard]] u64 InstanceId() const { return m_instanceId; }

    private:
        static u64 NextInstanceId()
        {
            static std::atomic<u64> counter{1};
            return counter.fetch_add(1, std::memory_order_relaxed);
        }
        u64 m_instanceId = 0;
    };

    /// Owns a CPU-side pixel buffer.
    class OwnedImageData : public ImageData
    {
    public:
        OwnedImageData() = default;

        /// Creates from a copy of the provided data.
        OwnedImageData(u32 w, u32 h, PixelFormat fmt, Span<const u8> data,
                       ImageColorSpace cs = ImageColorSpace::Srgb)
            : m_width(w), m_height(h), m_format(fmt), m_colorSpace(cs)
        {
            m_data.Resize(data.Size());
            if (data.Size() > 0)
            {
                std::memcpy(m_data.Data(), data.Data(), data.Size());
            }
        }

        /// Takes ownership of data by move.
        OwnedImageData(u32 w, u32 h, PixelFormat fmt, Array<u8>&& data,
                       ImageColorSpace cs = ImageColorSpace::Srgb)
            : m_width(w), m_height(h), m_format(fmt), m_colorSpace(cs),
              m_data(static_cast<Array<u8>&&>(data))
        {
        }

        [[nodiscard]] u32 Width() const override { return m_width; }
        [[nodiscard]] u32 Height() const override { return m_height; }
        [[nodiscard]] PixelFormat Format() const override { return m_format; }
        [[nodiscard]] ImageColorSpace ColorSpace() const override { return m_colorSpace; }
        [[nodiscard]] Span<const u8> PixelData() const override
        {
            return {m_data.Data(), m_data.Size()};
        }
        [[nodiscard]] Span<u8> PixelDataMut() { return {m_data.Data(), m_data.Size()}; }
        [[nodiscard]] u32 DataSize() const { return m_width * m_height * BytesPerPixel(m_format); }

    private:
        u32 m_width = 0, m_height = 0;
        PixelFormat m_format = PixelFormat::RGBA8;
        ImageColorSpace m_colorSpace = ImageColorSpace::Srgb;
        Array<u8> m_data;
    };

    /// References external pixel data (non-owning).
    /// Caller must ensure data outlives this reference.
    class ImageDataRef : public ImageData
    {
    public:
        ImageDataRef() = default;

        /// No pixel data (for GPU-managed textures).
        ImageDataRef(u32 w, u32 h, PixelFormat fmt = PixelFormat::RGBA8,
                     ImageColorSpace cs = ImageColorSpace::Srgb)
            : m_width(w), m_height(h), m_format(fmt), m_colorSpace(cs)
        {
        }

        /// Points to external pixel data.
        ImageDataRef(u32 w, u32 h, PixelFormat fmt, const u8* data, usize length,
                     ImageColorSpace cs = ImageColorSpace::Srgb)
            : m_width(w), m_height(h), m_format(fmt), m_colorSpace(cs), m_ptr(data),
              m_length(length)
        {
        }

        [[nodiscard]] u32 Width() const override { return m_width; }
        [[nodiscard]] u32 Height() const override { return m_height; }
        [[nodiscard]] PixelFormat Format() const override { return m_format; }
        [[nodiscard]] ImageColorSpace ColorSpace() const override { return m_colorSpace; }
        [[nodiscard]] Span<const u8> PixelData() const override
        {
            return m_ptr ? Span<const u8>(m_ptr, m_length) : Span<const u8>();
        }

    private:
        u32 m_width = 0, m_height = 0;
        PixelFormat m_format = PixelFormat::RGBA8;
        ImageColorSpace m_colorSpace = ImageColorSpace::Srgb;
        const u8* m_ptr = nullptr;
        usize m_length = 0;
    };

} // namespace draconic::image
