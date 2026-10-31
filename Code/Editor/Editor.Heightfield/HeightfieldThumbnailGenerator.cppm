// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor Heightfield - :thumbnail_generator partition
//
// The heightfield-domain thumbnail generator: a normalized grayscale relief view. Prepare
// (main thread) reads either the imported 16-bit heightmap image from the Sources/ mount or
// the authored "heights" sidecar (fields created on the page or saved by sculpting have no source
// file); Generate (light worker) min/max-normalizes the samples (low-relief maps would read
// near-black raw) and box-downscales into the tile.

module;
#include "Core/Prelude.h"

export module editor.heightfield:thumbnail_generator;

import foundation.core;
import foundation.content;
import foundation.image;
import foundation.image.io;
import foundation.vfs;
import editor.core;
import heightfield.pipeline;

using namespace foundation::core;
namespace content = foundation::content;
namespace image = foundation::image;

export namespace editor
{
    class HeightfieldThumbnailGenerator final : public editor::IThumbnailGenerator
    {
    public:
        [[nodiscard]] Span<const StringView> AssetTypeNames() const override
        {
            static constexpr StringView kTypes[] = {u8"HeightfieldAsset"};
            return Span<const StringView>(kTypes, 1);
        }

        [[nodiscard]] Status Prepare(content::Instance& instance,
                                     foundation::vfs::IFileSystem& sources,
                                     editor::ThumbnailPrepared& out) override
        {
            RefPtr<ISerializable> object = instance.ReadObject();
            auto* asset = Cast<pipeline::HeightfieldAsset>(object.Get());
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
            UniquePtr<IStream> heights = instance.ReadData(u8"heights");
            if (heights.Get() == nullptr)
            {
                return Status{ErrorCode::NotFound};
            }
            out.header.Clear();
            out.header.PushBack(byte{'R'});
            out.header.PushBack(byte{'1'});
            out.header.PushBack(byte{'6'});
            out.header.PushBack(byte{' '});
            const u32 side = static_cast<u32>(Max(asset->size, 1));
            for (u32 shift = 0; shift < 32; shift += 8)
            {
                out.header.PushBack(static_cast<byte>((side >> shift) & 0xff));
            }
            out.stream = Move(heights); // the samples follow the header (read on the worker)
            return Status{};
        }

        [[nodiscard]] Status Generate(Span<const byte> payload, image::Image& out) override
        {
            const u16* samples = nullptr;
            u32 width = 0;
            u32 height = 0;
            image::Image decoded;
            if (payload.Size() > 8 && payload[0] == byte{'R'} && payload[1] == byte{'1'} &&
                payload[2] == byte{'6'} && payload[3] == byte{' '})
            {
                const u32 side = static_cast<u32>(payload[4]) |
                                 (static_cast<u32>(payload[5]) << 8) |
                                 (static_cast<u32>(payload[6]) << 16) |
                                 (static_cast<u32>(payload[7]) << 24);
                const usize expected = static_cast<usize>(side) * side * 2;
                if (side == 0 || payload.Size() - 8 < expected)
                {
                    return Status{ErrorCode::InvalidArgument};
                }
                samples = reinterpret_cast<const u16*>(payload.Data() + 8);
                width = side;
                height = side;
            }
            else
            {
                const Status loaded = image::io::LoadImage16FromMemory(
                    Span<const u8>(reinterpret_cast<const u8*>(payload.Data()), payload.Size()),
                    decoded);
                if (!loaded.IsOk())
                {
                    return loaded;
                }
                samples = reinterpret_cast<const u16*>(decoded.PixelData().Data());
                width = decoded.Width();
                height = decoded.Height();
            }
            if (width == 0 || height == 0)
            {
                return Status{ErrorCode::InvalidArgument};
            }

            // Normalize over the full field so low-relief maps still show their shape.
            u16 minSample = 65535;
            u16 maxSample = 0;
            const usize count = static_cast<usize>(width) * height;
            for (usize i = 0; i < count; ++i)
            {
                minSample = Min(minSample, samples[i]);
                maxSample = Max(maxSample, samples[i]);
            }
            const u32 range = Max<u32>(1u, static_cast<u32>(maxSample) - minSample);

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
                    u64 sum = 0;
                    u32 n = 0;
                    for (u32 sy = y0; sy < y1; ++sy)
                    {
                        for (u32 sx = x0; sx < x1; ++sx)
                        {
                            sum += samples[static_cast<usize>(sy) * width + sx];
                            ++n;
                        }
                    }
                    const u32 normalized =
                        (static_cast<u32>(sum / n) - minSample) * 255u / range;
                    const u8 g = static_cast<u8>(Min(normalized, 255u));
                    u8* texel = dst.Data() + (static_cast<usize>(y) * kTile + x) * 4;
                    texel[0] = g;
                    texel[1] = g;
                    texel[2] = g;
                    texel[3] = 255;
                }
            }
            return Status{};
        }

    private:
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

    /// Registered by RegisterHeightfieldEditor (the domain's one composition entry point).
    inline void RegisterHeightfieldThumbnailGenerator(editor::ThumbnailService& service)
    {
        service.RegisterGenerator(MakeUnique<HeightfieldThumbnailGenerator>(editor::EditorRootAllocator()));
    }
}
