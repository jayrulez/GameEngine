// Draconic::TextureResource - the `draconic.texture.resource` module (runtime).
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
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.texture.resource;

import draconic.foundation;
import draconic.rhi;
import draconic.texture;
import draconic.content;
import draconic.resource;

using namespace draconic::foundation;
using namespace draconic::resource;

export namespace draconic::texture
{
    namespace rhi = draconic::rhi;

    // Cooked texture record (output DB). Pixels are the "data" stream.
    class TextureResource final : public ISerializable
    {
        DRACONIC_OBJECT(TextureResource, ISerializable)
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
            draconic::foundation::Serialize(ar, "width", width);
            draconic::foundation::Serialize(ar, "height", height);
            draconic::foundation::Serialize(ar, "depthOrArrayLayers", depthOrArrayLayers);
            draconic::foundation::Serialize(ar, "mipLevels", mipLevels);
            draconic::foundation::Serialize(ar, "format", format);
            draconic::foundation::Serialize(ar, "shape", shape);
            draconic::foundation::Serialize(ar, "minFilter", minFilter);
            draconic::foundation::Serialize(ar, "magFilter", magFilter);
            draconic::foundation::Serialize(ar, "wrapU", wrapU);
            draconic::foundation::Serialize(ar, "wrapV", wrapV);
            draconic::foundation::Serialize(ar, "wrapW", wrapW);
            draconic::foundation::Serialize(ar, "generateMipmaps", generateMipmaps);
            draconic::foundation::Serialize(ar, "anisotropy", anisotropy);
        }
    };

    // Runtime product: owns the live GPU texture + sampler.
    class Texture final : public Object
    {
        DRACONIC_OBJECT(Texture, Object)
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
        DRACONIC_OBJECT(DecodedTexture, Object)
    public:
        RefPtr<ISerializable> record; // the cooked TextureResource record
        Array<u8> pixels;             // cooked "data" stream bytes
    };

    // Cooked TextureResource -> live GPU Texture (model A). Device-backed.
    class TextureFactory final : public IResourceFactory
    {
    public:
        explicit TextureFactory(rhi::Device& device) noexcept : m_device(&device) {}

        [[nodiscard]] const TypeInfo* ProductType() const override
        {
            return &Texture::StaticType();
        }

        [[nodiscard]] RefPtr<Object> Create(ResourceManager& manager,
                                            draconic::content::Instance& instance) override
        {
            (void)manager;
            RefPtr<ISerializable> object = instance.ReadObject();
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

        [[nodiscard]] RefPtr<Object> DecodeStage(draconic::content::Instance& instance) override
        {
            RefPtr<DecodedTexture> decoded = MakeRef<DecodedTexture>(DefaultAllocator());
            decoded->record = instance.ReadObject();
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
        [[nodiscard]] static Array<u8> ReadCookedPixels(draconic::content::Instance& instance)
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
                    rhi::TextureDataLayout layout{};
                    layout.bytesPerRow = res->width * TextureData::GetBytesPerPixel(res->format);
                    layout.rowsPerImage = res->height;
                    if (isCube)
                    {
                        const usize faceBytes =
                            static_cast<usize>(layout.bytesPerRow) * res->height;
                        for (u32 face = 0; face < 6 && (face + 1) * faceBytes <= pixels.Size();
                             ++face)
                        {
                            batch->WriteTexture(
                                texture,
                                Span<const u8>(pixels.Data() + face * faceBytes, faceBytes), layout,
                                rhi::Extent3D{res->width, res->height, 1},
                                /*mipLevel*/ 0, /*arrayLayer*/ face);
                        }
                    }
                    else
                    {
                        batch->WriteTexture(texture, Span<const u8>(pixels.Data(), pixels.Size()),
                                            layout, rhi::Extent3D{res->width, res->height, 1});
                    }
                    (void)batch->Submit();
                    queue->DestroyTransferBatch(batch);
                }
            }

            rhi::SamplerDesc sd{};
            sd.minFilter = ToFilterMode(res->minFilter);
            sd.magFilter = ToFilterMode(res->magFilter);
            sd.mipmapFilter = (res->minFilter == TextureFilter::MipmapLinear)
                                  ? rhi::MipmapFilterMode::Linear
                                  : rhi::MipmapFilterMode::Nearest;
            sd.addressU = ToAddressMode(res->wrapU);
            sd.addressV = ToAddressMode(res->wrapV);
            sd.addressW = ToAddressMode(res->wrapW);
            sd.maxAnisotropy = static_cast<u16>(res->anisotropy < 1.0f ? 1.0f : res->anisotropy);
            rhi::Sampler* sampler = nullptr;
            (void)m_device->CreateSampler(sd, sampler);

            RefPtr<Texture> product = MakeRef<Texture>(DefaultAllocator());
            product->Adopt(m_device, texture, view, sampler, res->width, res->height, res->format,
                           isCube);
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

        rhi::Device* m_device;
    };

    inline void RegisterTextureResource()
    {
        GlobalTypeRegistry().Register(TextureResource::StaticType());
        RegisterSerializable<TextureResource>();
        // Force the async intermediate's type to initialize on the MAIN thread; DecodeStage
        // MakeRef<DecodedTexture>()s it on a worker, which must only ever read the type.
        (void)DecodedTexture::StaticType();
    }

    DRACONIC_DEFINE_OBJECT(TextureResource, "draconic::texture")
    DRACONIC_DEFINE_OBJECT(Texture, "draconic::texture")
    DRACONIC_DEFINE_OBJECT(DecodedTexture, "draconic::texture")
}
