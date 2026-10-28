// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Texture.Resource - the `foundation.texture.resource` module (runtime).
//
// The GPU texture as a runtime resource (model A, Traktor-style):
//   * TextureResource (ISerializable): the cooked *record* loaded from the output
//     DB - dims/RHI-format/mips/shape + sampler state. Cooked pixels live in the
//     "data" stream. This is what the factory reads (not bound directly).
//   * Texture (Object): the runtime product - owns the live rhi::Texture +
//     rhi::Sampler (created from the record + uploaded "data"). What a renderer
//     binds.
//   * TextureFactory (IResourceFactory): device-backed; cooked resource -> GPU
//     texture. Needs an rhi::Device (headless tests use the Null backend).
//
// The runtime never links the editor/source side; it loads only cooked resources.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.texture.resource;

import foundation.core;
import foundation.rhi;
import foundation.texture;
import foundation.content;
import foundation.resource;

using namespace foundation::core;
using namespace foundation::resource;

export namespace foundation::texture
{
    namespace rhi = foundation::rhi;

    // Cooked texture record (output DB). Pixels are the "data" stream.
    class TextureResource final : public ISerializable
    {
        RTTI_OBJECT(TextureResource, ISerializable)
    public:
        u32 width = 0;
        u32 height = 0;
        u32 depthOrArrayLayers = 1;
        u32 mipLevels = 1;
        rhi::TextureFormat format = rhi::TextureFormat::RGBA8Unorm;
        TextureShape shape = TextureShape::Texture2D;
        TextureFilter minFilter = TextureFilter::Linear;
        TextureFilter magFilter = TextureFilter::Linear;
        TextureWrap wrapU = TextureWrap::Repeat;
        TextureWrap wrapV = TextureWrap::Repeat;
        TextureWrap wrapW = TextureWrap::Repeat;
        bool generateMipmaps = true;
        f32 anisotropy = 1.0f;

        void Serialize(ISerializer& ar) override
        {
            foundation::core::Serialize(ar, "width", width);
            foundation::core::Serialize(ar, "height", height);
            foundation::core::Serialize(ar, "depthOrArrayLayers", depthOrArrayLayers);
            foundation::core::Serialize(ar, "mipLevels", mipLevels);
            foundation::core::Serialize(ar, "format", format);
            foundation::core::Serialize(ar, "shape", shape);
            foundation::core::Serialize(ar, "minFilter", minFilter);
            foundation::core::Serialize(ar, "magFilter", magFilter);
            foundation::core::Serialize(ar, "wrapU", wrapU);
            foundation::core::Serialize(ar, "wrapV", wrapV);
            foundation::core::Serialize(ar, "wrapW", wrapW);
            foundation::core::Serialize(ar, "generateMipmaps", generateMipmaps);
            foundation::core::Serialize(ar, "anisotropy", anisotropy);
        }
    };

    /// RenderTextureResource::kMaxSize, at namespace scope for another library to read (MSVC exports no class's
    /// static constexpr member from a shared library).
    inline constexpr u32 kRenderTextureResourceMaxSize = 4096;

    // Cooked render-texture record: a texture a camera renders into, with no pixels of its own.
    // Its product is an ordinary Texture, so whatever samples a texture samples this one.
    class RenderTextureResource final : public ISerializable
    {
        RTTI_OBJECT(RenderTextureResource, ISerializable)
    public:
        static constexpr u32 kMaxSize = kRenderTextureResourceMaxSize;

        u32 width = 256;
        u32 height = 256;
        rhi::TextureFormat format = rhi::TextureFormat::RGBA8UnormSrgb;

        /// A size the device can make and a format a camera can render to.
        [[nodiscard]] bool IsValid() const noexcept
        {
            return width >= 1 && height >= 1 && width <= kMaxSize && height <= kMaxSize &&
                   (format == rhi::TextureFormat::RGBA8UnormSrgb ||
                    format == rhi::TextureFormat::RGBA16Float);
        }

        void Serialize(ISerializer& ar) override
        {
            foundation::core::Serialize(ar, "width", width);
            foundation::core::Serialize(ar, "height", height);
            foundation::core::Serialize(ar, "format", format);
        }
    };

    // Runtime product: owns the live GPU texture + sampler.
    class Texture final : public Object
    {
        RTTI_OBJECT(Texture, Object)
    public:
        Texture() = default;
        ~Texture() override
        {
            if (m_device != nullptr)
            {
                if (m_view != nullptr)
                {
                    m_device->DestroyTextureView(m_view);
                }
                if (m_sampler != nullptr)
                {
                    m_device->DestroySampler(m_sampler);
                }
                if (m_texture != nullptr)
                {
                    m_device->DestroyTexture(m_texture);
                }
            }
        }
        Texture(const Texture&) = delete;
        Texture& operator=(const Texture&) = delete;

        void Adopt(rhi::Device* device, rhi::Texture* texture, rhi::TextureView* view,
                   rhi::Sampler* sampler, u32 width, u32 height, rhi::TextureFormat format,
                   bool isCube = false) noexcept
        {
            m_device = device;
            m_texture = texture;
            m_view = view;
            m_sampler = sampler;
            m_width = width;
            m_height = height;
            m_format = format;
            m_isCube = isCube;
            m_uid = NextUid(); // consumers key caches/dirty checks on this, never the pointer
        }

        /// Monotonic identity: a reloaded product is a NEW uid at (possibly) a reused address.
        [[nodiscard]] u64 Uid() const noexcept { return m_uid; }
        /// True when the default view is a cube (shape Cubemap - 6 faces).
        [[nodiscard]] bool IsCube() const noexcept { return m_isCube; }

        [[nodiscard]] rhi::Texture* GpuTexture() const noexcept { return m_texture; }
        [[nodiscard]] rhi::TextureView* View() const noexcept
        {
            return m_view;
        } // default sampled view (full mips)
        [[nodiscard]] rhi::Sampler* Sampler() const noexcept { return m_sampler; }
        [[nodiscard]] u32 Width() const noexcept { return m_width; }
        [[nodiscard]] u32 Height() const noexcept { return m_height; }
        [[nodiscard]] rhi::TextureFormat Format() const noexcept { return m_format; }

    private:
        [[nodiscard]] static u64 NextUid() noexcept
        {
            static Atomic<u64> counter{0};
            return counter.fetch_add(1) + 1;
        }

        rhi::Device* m_device = nullptr;    // non-owning
        rhi::Texture* m_texture = nullptr;  // owned (destroyed via device)
        rhi::TextureView* m_view = nullptr; // owned (default sampled view)
        rhi::Sampler* m_sampler = nullptr;  // owned
        u32 m_width = 0;
        u32 m_height = 0;
        rhi::TextureFormat m_format = rhi::TextureFormat::RGBA8Unorm;
        bool m_isCube = false;
        u64 m_uid = 0;
    };

    // Off-thread decode result for the async path (task #123): the parsed record + the raw cooked
    // pixel bytes, both read on a JobSystem worker (content-DB reads open independent streams and
    // the type/serializable registries are read-only during load, so this is concurrent-safe).
    // FinalizeStage turns it into the live GPU Texture on the main thread.
    class DecodedTexture final : public Object
    {
        RTTI_OBJECT(DecodedTexture, Object)
    public:
        RefPtr<ISerializable> record; // the cooked TextureResource record
        Array<u8> pixels;             // cooked "data" stream bytes
    };

    // Cooked TextureResource -> live GPU Texture (model A). Device-backed.
    /// One WriteTexture call of a cooked payload: the payload slice plus its layout, extent
    /// and target subresource.
    struct TextureWrite
    {
        usize offset = 0;
        usize size = 0;
        rhi::TextureDataLayout layout{};
        rhi::Extent3D extent{};
        u32 mipLevel = 0;
        u32 arrayLayer = 0;
    };

    /// Which slice of the payload goes to which mip level or cube face - the arithmetic that
    /// decides how far into a buffer to read, kept PURE so it is testable without a device.
    /// Cubemaps: six level-0 faces concatenated +X,-X,+Y,-Y,+Z,-Z (one layer write each).
    /// Otherwise: the concatenated mip chain, level sizes derived from the dims progression.
    /// Block-compressed formats measure by 4x4 block footprint, never per texel (there is no
    /// per-texel size; a per-texel face of 0 bytes silently uploaded nothing - the bug this
    /// fixes). A truncated payload yields only the writes that fit, never an overread.
    inline void EnumerateTextureWrites(const TextureResource& res, usize payloadBytes,
                                       Array<TextureWrite>& out)
    {
        out.Clear();
        const bool compressed = rhi::IsCompressed(res.format);
        const u32 bpp = TextureData::GetBytesPerPixel(res.format);
        const u32 blockH = rhi::BlockHeight(res.format);
        const auto layoutFor = [&](u32 w, u32 h)
        {
            rhi::TextureDataLayout layout{};
            layout.bytesPerRow = compressed ? rhi::CompressedRowPitch(res.format, w) : w * bpp;
            layout.rowsPerImage = compressed ? (h + blockH - 1) / blockH : h;
            return layout;
        };
        const auto bytesFor = [&](u32 w, u32 h) -> usize
        {
            return compressed ? rhi::CompressedLevelBytes(res.format, w, h)
                              : static_cast<usize>(w) * h * bpp;
        };

        if (res.shape == TextureShape::Cubemap)
        {
            const usize faceBytes = bytesFor(res.width, res.height);
            if (faceBytes == 0)
            {
                return;
            }
            for (u32 face = 0; face < 6 && (face + 1) * faceBytes <= payloadBytes; ++face)
            {
                TextureWrite w;
                w.offset = face * faceBytes;
                w.size = faceBytes;
                w.layout = layoutFor(res.width, res.height);
                w.extent = rhi::Extent3D{res.width, res.height, 1};
                w.mipLevel = 0;
                w.arrayLayer = face;
                out.PushBack(w);
            }
            return;
        }

        usize offset = 0;
        u32 levelW = res.width;
        u32 levelH = res.height;
        for (u32 level = 0; level < res.mipLevels; ++level)
        {
            const usize levelBytes = bytesFor(levelW, levelH);
            if (levelBytes == 0 || offset + levelBytes > payloadBytes)
            {
                break; // truncated payload: upload what exists, never overread
            }
            TextureWrite w;
            w.offset = offset;
            w.size = levelBytes;
            w.layout = layoutFor(levelW, levelH);
            w.extent = rhi::Extent3D{levelW, levelH, 1};
            w.mipLevel = level;
            w.arrayLayer = 0;
            out.PushBack(w);
            offset += levelBytes;
            levelW = levelW > 1 ? levelW / 2 : 1;
            levelH = levelH > 1 ? levelH / 2 : 1;
        }
    }

    class TextureFactory final : public IResourceFactory
    {
    public:
        // The allocator backs every product this factory creates (required -
        // the application that registers the factory decides).
        TextureFactory(IAllocator& allocator, rhi::Device& device) noexcept
            : m_allocator(&allocator), m_device(&device)
        {
        }

        [[nodiscard]] const TypeInfo* ProductType() const override
        {
            return &Texture::StaticType();
        }
        [[nodiscard]] const TypeInfo* CookedType() const override
        {
            return &TextureResource::StaticType();
        }

        [[nodiscard]] RefPtr<Object> Create(ResourceManager& manager,
                                            foundation::content::Instance& instance) override
        {
            (void)manager;
            RefPtr<ISerializable> object = instance.ReadObject();
            if (const RenderTextureResource* target = Cast<RenderTextureResource>(object.Get()))
            {
                return BuildRenderTexture(*target);
            }
            TextureResource* res = Cast<TextureResource>(object.Get());
            if (res == nullptr)
            {
                return RefPtr<Object>{};
            }
            const Array<u8> pixels = ReadCookedPixels(instance);
            return BuildTexture(*res, pixels);
        }

        // --- async path (task #123): decode (record + pixel bytes) on a worker, upload on main ---
        [[nodiscard]] bool SupportsAsync() const override { return true; }

        [[nodiscard]] RefPtr<Object> DecodeStage(foundation::content::Instance& instance) override
        {
            RefPtr<DecodedTexture> decoded = MakeRef<DecodedTexture>((*m_allocator));
            decoded->record = instance.ReadObject();
            if (Cast<RenderTextureResource>(decoded->record.Get()) != nullptr)
            {
                return decoded; // no pixels: the camera that targets it draws them
            }
            if (Cast<TextureResource>(decoded->record.Get()) == nullptr)
            {
                return RefPtr<Object>{}; // not a texture record -> decode failure
            }
            decoded->pixels = ReadCookedPixels(instance);
            return decoded;
        }

        [[nodiscard]] RefPtr<Object> FinalizeStage(ResourceManager& manager,
                                                   RefPtr<Object> decoded) override
        {
            (void)manager;
            DecodedTexture* d = Cast<DecodedTexture>(decoded.Get());
            if (d == nullptr)
            {
                return RefPtr<Object>{};
            }
            if (const RenderTextureResource* target = Cast<RenderTextureResource>(d->record.Get()))
            {
                return BuildRenderTexture(*target);
            }
            TextureResource* res = Cast<TextureResource>(d->record.Get());
            if (res == nullptr)
            {
                return RefPtr<Object>{};
            }
            return BuildTexture(*res, d->pixels);
        }

    private:
        // Read the cooked "data" stream (heavy pixel bytes). Pure: opens an independent stream, so
        // it is safe to call from a worker (see DecodedTexture).
        [[nodiscard]] static Array<u8> ReadCookedPixels(foundation::content::Instance& instance)
        {
            Array<u8> pixels;
            if (UniquePtr<IStream> stream = instance.ReadData(u8"data"))
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
            return pixels;
        }

        // Create the live GPU texture/view/sampler and upload the pixels. MAIN THREAD ONLY (RHI).
        [[nodiscard]] RefPtr<Object> BuildTexture(const TextureResource& record,
                                                  const Array<u8>& pixels)
        {
            const TextureResource* res = &record;
            const bool isCube = (res->shape == TextureShape::Cubemap);

            rhi::TextureDesc desc{};
            desc.dimension = rhi::TextureDimension::Texture2D;
            desc.format = res->format;
            desc.width = res->width;
            desc.height = res->height;
            desc.depth = 1;
            desc.arrayLayerCount = isCube ? 6u : res->depthOrArrayLayers;
            desc.mipLevelCount = res->mipLevels;
            desc.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::CopyDst;

            rhi::Texture* texture = nullptr;
            if (!m_device->CreateTexture(desc, texture).IsOk())
            {
                return RefPtr<Object>{};
            }

            // Default sampled view spanning all mips/layers - the currency the material/renderer bind.
            // Cube-shaped assets (skyboxes) get a real TextureCube view so consumers can sample it as one.
            rhi::TextureViewDesc vd{};
            vd.format = res->format;
            vd.dimension = isCube ? rhi::TextureViewDimension::TextureCube
                                  : rhi::TextureViewDimension::Texture2D;
            vd.mipLevelCount = res->mipLevels;
            vd.arrayLayerCount = isCube ? 6u : res->depthOrArrayLayers;
            rhi::TextureView* view = nullptr;
            if (!m_device->CreateTextureView(texture, vd, view).IsOk())
            {
                m_device->DestroyTexture(texture);
                return RefPtr<Object>{};
            }

            // Upload mip 0 via a transfer batch (cubes: the cooked stream is the 6 faces
            // concatenated +X,-X,+Y,-Y,+Z,-Z - one layer write each).
            if (!pixels.IsEmpty())
            {
                rhi::Queue* queue = m_device->GetQueue(rhi::QueueType::Graphics, 0);
                rhi::TransferBatch* batch = nullptr;
                if (queue != nullptr && queue->CreateTransferBatch(batch).IsOk() &&
                    batch != nullptr)
                {
                    Array<TextureWrite> writes;
                    EnumerateTextureWrites(*res, pixels.Size(), writes);
                    for (const TextureWrite& w : writes)
                    {
                        batch->WriteTexture(texture, Span<const u8>(pixels.Data() + w.offset, w.size),
                                            w.layout, w.extent, w.mipLevel, w.arrayLayer);
                    }
                    (void)batch->Submit();
                    queue->DestroyTransferBatch(batch);
                }
            }

            rhi::SamplerDesc sd{};
            sd.minFilter = ToFilterMode(res->minFilter);
            sd.magFilter = ToFilterMode(res->magFilter);
            // Trilinear whenever a chain exists: model imports say minFilter=Linear, and
            // nearest-mip banding on a real chain is never what anyone wants.
            sd.mipmapFilter = (res->mipLevels > 1 || res->minFilter == TextureFilter::MipmapLinear)
                                  ? rhi::MipmapFilterMode::Linear
                                  : rhi::MipmapFilterMode::Nearest;
            sd.addressU = ToAddressMode(res->wrapU);
            sd.addressV = ToAddressMode(res->wrapV);
            sd.addressW = ToAddressMode(res->wrapW);
            sd.maxAnisotropy = static_cast<u16>(res->anisotropy < 1.0f ? 1.0f : res->anisotropy);
            rhi::Sampler* sampler = nullptr;
            (void)m_device->CreateSampler(sd, sampler);

            RefPtr<Texture> product = MakeRef<Texture>((*m_allocator));
            product->Adopt(m_device, texture, view, sampler, res->width, res->height, res->format,
                           isCube);
            return product;
        }

        // A texture a camera renders into and anything samples: render-target and sampled, plus
        // CopySrc so a capture can read it out. It starts cleared to transparent black through
        // an upload, which also leaves it in the shader-read state, so a texture sampled before
        // its camera has drawn (an inactive camera, the first frame) is defined. MAIN THREAD ONLY.
        [[nodiscard]] RefPtr<Object> BuildRenderTexture(const RenderTextureResource& record)
        {
            if (!record.IsValid())
            {
                return RefPtr<Object>{};
            }
            rhi::TextureDesc desc = rhi::TextureDesc::RenderTarget(record.format, record.width,
                                                                   record.height, 1,
                                                                   u8"RenderTexture");
            desc.usage = desc.usage | rhi::TextureUsage::CopySrc | rhi::TextureUsage::CopyDst;
            rhi::Texture* texture = nullptr;
            if (!m_device->CreateTexture(desc, texture).IsOk())
            {
                return RefPtr<Object>{};
            }
            rhi::TextureViewDesc vd{};
            vd.format = record.format;
            vd.dimension = rhi::TextureViewDimension::Texture2D;
            rhi::TextureView* view = nullptr;
            if (!m_device->CreateTextureView(texture, vd, view).IsOk())
            {
                m_device->DestroyTexture(texture);
                return RefPtr<Object>{};
            }

            const u32 bytesPerPixel = rhi::BytesPerPixel(record.format);
            Array<u8> clear;
            clear.Resize(static_cast<usize>(record.width) * record.height * bytesPerPixel, 0);
            rhi::Queue* queue = m_device->GetQueue(rhi::QueueType::Graphics, 0);
            rhi::TransferBatch* batch = nullptr;
            if (queue != nullptr && queue->CreateTransferBatch(batch).IsOk() && batch != nullptr)
            {
                rhi::TextureDataLayout layout{};
                layout.bytesPerRow = record.width * bytesPerPixel;
                layout.rowsPerImage = record.height;
                batch->WriteTexture(texture, Span<const u8>(clear.Data(), clear.Size()), layout,
                                    rhi::Extent3D{record.width, record.height, 1}, 0, 0);
                (void)batch->Submit();
                queue->DestroyTransferBatch(batch);
            }

            rhi::SamplerDesc sd{};
            sd.minFilter = rhi::FilterMode::Linear;
            sd.magFilter = rhi::FilterMode::Linear;
            sd.addressU = rhi::AddressMode::ClampToEdge;
            sd.addressV = rhi::AddressMode::ClampToEdge;
            sd.addressW = rhi::AddressMode::ClampToEdge;
            rhi::Sampler* sampler = nullptr;
            (void)m_device->CreateSampler(sd, sampler);

            RefPtr<Texture> product = MakeRef<Texture>((*m_allocator));
            product->Adopt(m_device, texture, view, sampler, record.width, record.height,
                           record.format);
            return product;
        }

    private:
        [[nodiscard]] static rhi::FilterMode ToFilterMode(TextureFilter f)
        {
            return (f == TextureFilter::Nearest || f == TextureFilter::MipmapNearest)
                       ? rhi::FilterMode::Nearest
                       : rhi::FilterMode::Linear;
        }
        [[nodiscard]] static rhi::AddressMode ToAddressMode(TextureWrap w)
        {
            switch (w)
            {
            case TextureWrap::Repeat:
                return rhi::AddressMode::Repeat;
            case TextureWrap::ClampToEdge:
                return rhi::AddressMode::ClampToEdge;
            case TextureWrap::ClampToBorder:
                return rhi::AddressMode::ClampToBorder;
            case TextureWrap::MirroredRepeat:
                return rhi::AddressMode::MirrorRepeat;
            }
            return rhi::AddressMode::Repeat;
        }

        IAllocator* m_allocator;
        rhi::Device* m_device;
    };

    inline void RegisterTextureResource()
    {
        GlobalTypeRegistry().Register(TextureResource::StaticType());
        RegisterSerializable<TextureResource>();
        GlobalTypeRegistry().Register(RenderTextureResource::StaticType());
        RegisterSerializable<RenderTextureResource>();
        // Force the async intermediate's type to initialize on the MAIN thread; DecodeStage
        // MakeRef<DecodedTexture>()s it on a worker, which must only ever read the type.
        (void)DecodedTexture::StaticType();
    }

    RTTI_DEFINE_OBJECT(TextureResource, "rtti::texture")
    RTTI_DEFINE_OBJECT(RenderTextureResource, "rtti::texture")
    RTTI_DEFINE_OBJECT(Texture, "rtti::texture")
    RTTI_DEFINE_OBJECT(DecodedTexture, "rtti::texture")
}

export namespace foundation::texture
{
    /// The texture resource module (engine-composition.md D1): the module the engine
    /// composition composes this library's factories from.
    /// What the texture factory reads beside a TextureResource: a render texture's record.
    [[nodiscard]] inline const TypeInfo* TextureAlsoCooked(usize index)
    {
        return (index == 0) ? &RenderTextureResource::StaticType() : nullptr;
    }

    inline constexpr foundation::resource::ResourceFactoryDesc kTextureResourceFactories[] = {
        []() constexpr
        {
            foundation::resource::ResourceFactoryDesc desc =
                foundation::resource::FactoryWithService<Texture, TextureResource, TextureFactory,
                                                         foundation::rhi::Device>();
            desc.alsoCooked = &TextureAlsoCooked;
            return desc;
        }(),
    };
    inline constexpr foundation::resource::ResourceModule kTextureResourceModule{
        u8"texture", &RegisterTextureResource, kTextureResourceFactories,
        sizeof(kTextureResourceFactories) / sizeof(kTextureResourceFactories[0])};
}
