// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor Texture - :thumbnail_generator partition
//
// The texture-domain thumbnail generator. Lives in the DOMAIN lib:
// generators belong beside the asset type's editor surface, which
// already owns the pipeline knowledge - Editor.App links no pipeline libs for thumbnails, and
// registration rides the domain's RegisterTextureEditor call from the Tools.Editor composition
// root. TextureThumbnailGenerator:
// Prepare (main thread) reads the TextureAsset's source bytes - external file stream (named by
// fileName) or the embedded "pixels" stream from model imports; Generate (light worker) decodes,
// aspect-fit box-downscales into a 128x128 RGBA8 tile, and composites a checkerboard behind
// translucent texels (the Traktor touch: alpha reads as alpha, not as darkness). GPU-rendered
// previews (mesh/material/prefab/scene) are the preview-bake path, not generators here.

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"

export module editor.texture:thumbnail_generator;

import foundation.core;
import foundation.content;
import foundation.image;
import foundation.image.io;
import foundation.vfs;
import editor.core;
import texture.pipeline;

using namespace foundation::core;
namespace content = foundation::content;
namespace image = foundation::image;

export namespace editor
{
    class TextureThumbnailGenerator final : public editor::IThumbnailGenerator
    {
    public:
        [[nodiscard]] Span<const StringView> AssetTypeNames() const override
        {
            static constexpr StringView kTypes[] = {u8"TextureAsset"};
            return Span<const StringView>(kTypes, 1);
        }

        [[nodiscard]] Status Prepare(content::Instance& instance,
                                     foundation::vfs::IFileSystem& sources,
                                     editor::ThumbnailPrepared& out) override
        {
            RefPtr<ISerializable> object = instance.ReadObject();
            auto* asset = Cast<pipeline::TextureAsset>(object.Get());
            if (asset == nullptr)
            {
                return Status{ErrorCode::InvalidArgument};
            }
            if (!asset->fileName.IsEmpty())
            {
                UniquePtr<IStream> stream =
                    sources.Open(asset->fileName.View(), FileMode::Read);
                if (stream.Get() != nullptr && stream->IsValid())
                {
                    out.stream = Move(stream); // the worker reads the encoded file
                    return Status{};
                }
            }
            if (asset->embeddedWidth > 0 && asset->embeddedHeight > 0)
            {
                UniquePtr<IStream> stream = instance.ReadData(u8"pixels");
                if (stream.Get() == nullptr)
                {
                    return Status{ErrorCode::NotFound};
                }
                out.header.Clear();
                out.header.PushBack(byte{'R'});
                out.header.PushBack(byte{'A'});
                out.header.PushBack(byte{'W'});
                out.header.PushBack(byte{'8'});
                AppendU32(out.header, static_cast<u32>(asset->embeddedWidth));
                AppendU32(out.header, static_cast<u32>(asset->embeddedHeight));
                out.stream = Move(stream); // the pixels follow the header
                return Status{};
            }
            return Status{ErrorCode::NotFound};
        }

        [[nodiscard]] Status Generate(Span<const byte> payload, image::Image& out) override
        {
            image::Image source;
            if (payload.Size() > 12 && payload[0] == byte{'R'} && payload[1] == byte{'A'} &&
                payload[2] == byte{'W'} && payload[3] == byte{'8'})
            {
                const u32 width = ReadU32(payload, 4);
                const u32 height = ReadU32(payload, 8);
                const usize expected = static_cast<usize>(width) * height * 4;
                if (width == 0 || height == 0 || payload.Size() - 12 < expected)
                {
                    return Status{ErrorCode::InvalidArgument};
                }
                source = image::Image(
                    width, height, image::PixelFormat::RGBA8,
                    Span<const u8>(reinterpret_cast<const u8*>(payload.Data()) + 12, expected));
            }
            else
            {
                const Status loaded = image::io::LoadImageFromMemory(
                    Span<const u8>(reinterpret_cast<const u8*>(payload.Data()), payload.Size()),
                    source);
                if (!loaded.IsOk())
                {
                    return loaded;
                }
            }
            return DownscaleIntoTile(source, out);
        }

    private:
        static void AppendU32(Array<byte>& payload, u32 value)
        {
            payload.PushBack(static_cast<byte>(value & 0xff));
            payload.PushBack(static_cast<byte>((value >> 8) & 0xff));
            payload.PushBack(static_cast<byte>((value >> 16) & 0xff));
            payload.PushBack(static_cast<byte>((value >> 24) & 0xff));
        }
        [[nodiscard]] static u32 ReadU32(Span<const byte> payload, usize at)
        {
            return static_cast<u32>(payload[at]) | (static_cast<u32>(payload[at + 1]) << 8) |
                   (static_cast<u32>(payload[at + 2]) << 16) |
                   (static_cast<u32>(payload[at + 3]) << 24);
        }

        [[nodiscard]] static Status ReadAll(IStream& stream, Array<byte>& out)
        {
            const i64 size = stream.Size();
            if (size <= 0)
            {
                return Status{ErrorCode::NotFound};
            }
            out.Resize(static_cast<usize>(size));
            const u64 read = stream.Read(out.Data(), static_cast<u64>(size));
            return read == static_cast<u64>(size) ? Status{} : Status{ErrorCode::Internal};
        }

        /// Aspect-fit box downscale into the square tile, checkerboard composited behind any
        /// translucency, transparent letterbox padding. Pure CPU (light worker).
        [[nodiscard]] static Status DownscaleIntoTile(const image::Image& source,
                                                      image::Image& out)
        {
            if (source.Format() != image::PixelFormat::RGBA8)
            {
                return Status{ErrorCode::InvalidArgument}; // loaders always yield RGBA8 today
            }
            constexpr u32 kTile = editor::kThumbnailServiceThumbnailSize;
            const u32 sourceW = source.Width();
            const u32 sourceH = source.Height();
            if (sourceW == 0 || sourceH == 0)
            {
                return Status{ErrorCode::InvalidArgument};
            }
            const f32 scale =
                Min(static_cast<f32>(kTile) / sourceW, static_cast<f32>(kTile) / sourceH);
            const u32 fitW = Max(1u, static_cast<u32>(sourceW * scale));
            const u32 fitH = Max(1u, static_cast<u32>(sourceH * scale));
            const u32 offsetX = (kTile - fitW) / 2;
            const u32 offsetY = (kTile - fitH) / 2;

            out = image::Image(kTile, kTile, image::PixelFormat::RGBA8);
            Span<u8> dst = out.PixelDataMut(); // Image() clears to zero = transparent letterbox
            const Span<const u8> src = source.PixelData();

            for (u32 y = 0; y < fitH; ++y)
            {
                const u32 y0 = y * sourceH / fitH;
                const u32 y1 = Max(y0 + 1, (y + 1) * sourceH / fitH);
                for (u32 x = 0; x < fitW; ++x)
                {
                    const u32 x0 = x * sourceW / fitW;
                    const u32 x1 = Max(x0 + 1, (x + 1) * sourceW / fitW);
                    u32 r = 0, g = 0, b = 0, a = 0, n = 0;
                    for (u32 sy = y0; sy < y1; ++sy)
                    {
                        for (u32 sx = x0; sx < x1; ++sx)
                        {
                            const usize p = (static_cast<usize>(sy) * sourceW + sx) * 4;
                            r += src[p + 0];
                            g += src[p + 1];
                            b += src[p + 2];
                            a += src[p + 3];
                            ++n;
                        }
                    }
                    r /= n;
                    g /= n;
                    b /= n;
                    a /= n;
                    // Checkerboard behind translucency so alpha reads as alpha (8px cells).
                    if (a < 255)
                    {
                        const u8 check = (((x / 8) + (y / 8)) % 2 == 0) ? 200 : 128;
                        r = (r * a + check * (255 - a)) / 255;
                        g = (g * a + check * (255 - a)) / 255;
                        b = (b * a + check * (255 - a)) / 255;
                        a = 255;
                    }
                    const usize q =
                        (static_cast<usize>(y + offsetY) * kTile + (x + offsetX)) * 4;
                    dst[q + 0] = static_cast<u8>(r);
                    dst[q + 1] = static_cast<u8>(g);
                    dst[q + 2] = static_cast<u8>(b);
                    dst[q + 3] = static_cast<u8>(a);
                }
            }
            return Status{};
        }
    };

    /// Registered by RegisterTextureEditor (the domain's one composition entry point).
    inline void RegisterTextureThumbnailGenerator(editor::ThumbnailService& service)
    {
        service.RegisterGenerator(MakeUnique<TextureThumbnailGenerator>(editor::EditorRootAllocator()));
    }
}
