// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor Terrain - :splatmap_thumbnail partition
//
// The splatmap thumbnail generator: a false-color view of the slot weights. A splat raster holds
// weights, not color, and what channel k means depends on the per-texel palette indices. So the
// tile maps the four slots to fixed distinct hues and blends them by weight; the shape of the
// painted regions is what makes it recognizable, not the layer colors. Prepare reads the imported
// source PNG (a fixed-layer raster) or the painted "pixels" sidecar; Generate colorizes and
// box-downscales.

module;
#include "Core/Prelude.h"

export module editor.terrain:splatmap_thumbnail;

import foundation.core;
import foundation.content;
import foundation.image;
import foundation.image.io;
import foundation.vfs;
import editor.core;
import terrain.pipeline;

using namespace foundation::core;
namespace content = foundation::content;
namespace image = foundation::image;

export namespace editor
{
    class SplatmapThumbnailGenerator final : public editor::IThumbnailGenerator
    {
    public:
        [[nodiscard]] Span<const StringView> AssetTypeNames() const override
        {
            static constexpr StringView kTypes[] = {u8"SplatmapAsset"};
            return Span<const StringView>(kTypes, 1);
        }

        [[nodiscard]] Status Prepare(content::Instance& instance,
                                     foundation::vfs::IFileSystem& sources,
                                     editor::ThumbnailPrepared& out) override
        {
            RefPtr<ISerializable> object = instance.ReadObject();
            auto* asset = Cast<pipeline::SplatmapAsset>(object.Get());
            if (asset == nullptr)
            {
                return Status{ErrorCode::InvalidArgument};
            }
            if (!asset->fileName.IsEmpty())
            {
                UniquePtr<IStream> stream = sources.Open(asset->fileName.View(), FileMode::Read);
                if (stream.Get() != nullptr && stream->IsValid())
                {
                    out.stream = Move(stream); // encoded image bytes, no header
                    return Status{};
                }
            }
            UniquePtr<IStream> weights = instance.ReadData(u8"pixels");
            if (weights.Get() == nullptr)
            {
                return Status{ErrorCode::NotFound};
            }
            out.header.Clear();
            out.header.PushBack(byte{'S'});
            out.header.PushBack(byte{'P'});
            out.header.PushBack(byte{'L'});
            out.header.PushBack(byte{'T'});
            AppendU32(out.header, static_cast<u32>(Max(asset->width, 1)));
            AppendU32(out.header, static_cast<u32>(Max(asset->height, 1)));
            out.stream = Move(weights); // the raster follows the header (read on the worker)
            return Status{};
        }

        [[nodiscard]] Status Generate(Span<const byte> payload, image::Image& out) override
        {
            const u8* weights = nullptr;
            u32 width = 0;
            u32 height = 0;
            image::Image decoded;
            if (payload.Size() > 12 && payload[0] == byte{'S'} && payload[1] == byte{'P'} &&
                payload[2] == byte{'L'} && payload[3] == byte{'T'})
            {
                width = ReadU32(payload, 4);
                height = ReadU32(payload, 8);
                const usize expected = static_cast<usize>(width) * height * 4;
                if (width == 0 || height == 0 || payload.Size() - 12 < expected)
                {
                    return Status{ErrorCode::InvalidArgument};
                }
                weights = reinterpret_cast<const u8*>(payload.Data()) + 12;
            }
            else
            {
                const Status loaded = image::io::LoadImageFromMemory(
                    Span<const u8>(reinterpret_cast<const u8*>(payload.Data()), payload.Size()),
                    decoded);
                if (!loaded.IsOk())
                {
                    return loaded;
                }
                if (decoded.Format() != image::PixelFormat::RGBA8)
                {
                    return Status{ErrorCode::InvalidArgument};
                }
                weights = decoded.PixelData().Data();
                width = decoded.Width();
                height = decoded.Height();
            }

            // Fixed slot hues (weights blend by coverage; slot 0 is the base layer).
            static constexpr u8 kSlotColor[4][3] = {
                {96, 128, 72},   // moss
                {168, 138, 92},  // sand
                {110, 116, 128}, // rock
                {196, 186, 74},  // gold
            };

            constexpr u32 kTile = editor::kThumbnailServiceThumbnailSize;
            out = image::Image(kTile, kTile, image::PixelFormat::RGBA8);
            Span<u8> dst = out.PixelDataMut();
            for (u32 y = 0; y < kTile; ++y)
            {
                const u32 y0 = y * height / kTile;
                const u32 y1 = Max(y0 + 1, (y + 1) * height / kTile);
                for (u32 x = 0; x < kTile; ++x)
                {
                    const u32 x0 = x * width / kTile;
                    const u32 x1 = Max(x0 + 1, (x + 1) * width / kTile);
                    u32 slot[4] = {0, 0, 0, 0};
                    u32 n = 0;
                    for (u32 sy = y0; sy < y1; ++sy)
                    {
                        for (u32 sx = x0; sx < x1; ++sx)
                        {
                            const usize p = (static_cast<usize>(sy) * width + sx) * 4;
                            for (u32 k = 0; k < 4; ++k)
                            {
                                slot[k] += weights[p + k];
                            }
                            ++n;
                        }
                    }
                    u32 total = 0;
                    u32 color[3] = {0, 0, 0};
                    for (u32 k = 0; k < 4; ++k)
                    {
                        const u32 w = slot[k] / n;
                        total += w;
                        for (u32 c = 0; c < 3; ++c)
                        {
                            color[c] += kSlotColor[k][c] * w;
                        }
                    }
                    u8* texel = dst.Data() + (static_cast<usize>(y) * kTile + x) * 4;
                    for (u32 c = 0; c < 3; ++c)
                    {
                        texel[c] = static_cast<u8>(total > 0 ? Min(color[c] / total, 255u) : 30u);
                    }
                    texel[3] = 255;
                }
            }
            return Status{};
        }

    private:
        static void AppendU32(Array<byte>& payload, u32 value)
        {
            for (u32 shift = 0; shift < 32; shift += 8)
            {
                payload.PushBack(static_cast<byte>((value >> shift) & 0xff));
            }
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
    };

    /// Registered by RegisterTerrainEditor, the domain's single composition entry point.
    inline void RegisterSplatmapThumbnailGenerator(editor::ThumbnailService& service)
    {
        service.RegisterGenerator(MakeUnique<SplatmapThumbnailGenerator>(editor::EditorRootAllocator()));
    }
}
