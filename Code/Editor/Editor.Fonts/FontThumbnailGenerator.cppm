// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor Fonts - :thumbnail_generator partition
//
// The font-domain thumbnail generator: a rasterized "Ag" sample. Prepare (main thread) reads
// the source TTF/OTF bytes from the Sources/ mount; Generate (light worker) raster-bakes just
// the needed codepoints at a large pixel size (always coverage - the distance-field mode is a
// runtime concern, a sample glyph reads the same either way), lays the pair on a shared
// baseline, and composites light ink over the stage's dark tile ground.

module;
#include "Core/Prelude.h"

export module editor.fonts:thumbnail_generator;

import foundation.core;
import foundation.content;
import foundation.image;
import foundation.vfs;
import foundation.fonts;
import foundation.fonts.coverage.baker;
import editor.core;
import fonts.pipeline;

using namespace foundation::core;
namespace content = foundation::content;
namespace image = foundation::image;
namespace fonts = foundation::fonts;

export namespace editor
{
    class FontThumbnailGenerator final : public editor::IThumbnailGenerator
    {
    public:
        [[nodiscard]] Span<const StringView> AssetTypeNames() const override
        {
            static constexpr StringView kTypes[] = {u8"FontAsset"};
            return Span<const StringView>(kTypes, 1);
        }

        [[nodiscard]] Status Prepare(content::Instance& instance,
                                     foundation::vfs::IFileSystem& sources,
                                     editor::ThumbnailPrepared& out) override
        {
            RefPtr<ISerializable> object = instance.ReadObject();
            auto* asset = Cast<pipeline::FontAsset>(object.Get());
            if (asset == nullptr || asset->fileName.IsEmpty())
            {
                return Status{ErrorCode::InvalidArgument};
            }
            UniquePtr<IStream> stream = sources.Open(asset->fileName.View(), FileMode::Read);
            if (stream.Get() == nullptr || !stream->IsValid() || stream->Size() <= 0)
            {
                return Status{ErrorCode::NotFound};
            }
            out.stream = Move(stream); // the worker reads the file
            return Status{};
        }

        [[nodiscard]] Status Generate(Span<const byte> payload, image::Image& out) override
        {
            // Bake only 'A'..'g' at sample size; oversample 1 keeps atlas regions 1:1 with
            // logical pixels so they blit directly.
            fonts::FontLoadOptions options;
            options.pixelHeight = 72.0f;
            options.firstCodepoint = 'A';
            options.lastCodepoint = 'g';
            options.atlasWidth = 512;
            options.atlasHeight = 512;
            options.oversampleX = 1;
            options.oversampleY = 1;
            Result<fonts::BakedFontData*, fonts::FontLoadResult> baked =
                fonts::FontBaker::Bake(
                    Span<const u8>(reinterpret_cast<const u8*>(payload.Data()), payload.Size()),
                    options, editor::EditorRootAllocator());
            if (!baked.HasValue())
            {
                return Status{ErrorCode::InvalidArgument};
            }
            UniquePtr<fonts::BakedFontData> data(baked.Value(), editor::EditorRootAllocator());

            constexpr i32 kSample[] = {'A', 'g'};
            fonts::AtlasRegion regions[2];
            for (usize i = 0; i < 2; ++i)
            {
                if (!data->atlas->TryGetRegion(data->atlas->GlyphFor(kSample[i]), regions[i]))
                {
                    return Status{ErrorCode::NotFound};
                }
            }

            // Union of the placed glyph rects (pen advances along a y=0 baseline), so the
            // pair centers as a block regardless of the face's metrics.
            f32 pen = 0.0f;
            f32 minX = 0.0f, minY = 0.0f, maxX = 0.0f, maxY = 0.0f;
            for (usize i = 0; i < 2; ++i)
            {
                const fonts::AtlasRegion& r = regions[i];
                const f32 x0 = pen + r.offsetX;
                const f32 y0 = r.offsetY; // y-down, negative above the baseline
                minX = (i == 0) ? x0 : Min(minX, x0);
                minY = (i == 0) ? y0 : Min(minY, y0);
                maxX = Max(maxX, x0 + r.width);
                maxY = Max(maxY, y0 + r.height);
                pen += r.advanceX;
            }

            constexpr u32 kTile = editor::kThumbnailServiceThumbnailSize;
            constexpr u8 kGround[3] = {26, 28, 33};  // the GPU stage's tile ground
            constexpr u8 kInk[3] = {225, 227, 232};
            out = image::Image(kTile, kTile, image::PixelFormat::RGBA8);
            Span<u8> dst = out.PixelDataMut();
            for (usize i = 0; i < dst.Size(); i += 4)
            {
                dst[i + 0] = kGround[0];
                dst[i + 1] = kGround[1];
                dst[i + 2] = kGround[2];
                dst[i + 3] = 255;
            }

            const i32 shiftX =
                static_cast<i32>((kTile - (maxX - minX)) * 0.5f - minX);
            const i32 shiftY =
                static_cast<i32>((kTile - (maxY - minY)) * 0.5f - minY);
            const Span<const u8> coverage = data->atlas->PixelData();
            const u32 atlasW = data->atlas->Width();

            pen = 0.0f;
            for (usize i = 0; i < 2; ++i)
            {
                const fonts::AtlasRegion& r = regions[i];
                const i32 destX = static_cast<i32>(pen + r.offsetX) + shiftX;
                const i32 destY = static_cast<i32>(r.offsetY) + shiftY;
                for (u32 sy = 0; sy < r.height; ++sy)
                {
                    const i32 y = destY + static_cast<i32>(sy);
                    if (y < 0 || y >= static_cast<i32>(kTile))
                    {
                        continue;
                    }
                    for (u32 sx = 0; sx < r.width; ++sx)
                    {
                        const i32 x = destX + static_cast<i32>(sx);
                        if (x < 0 || x >= static_cast<i32>(kTile))
                        {
                            continue;
                        }
                        const u32 alpha =
                            coverage[static_cast<usize>(r.y + sy) * atlasW + (r.x + sx)];
                        if (alpha == 0)
                        {
                            continue;
                        }
                        u8* texel = dst.Data() + (static_cast<usize>(y) * kTile + x) * 4;
                        for (u32 c = 0; c < 3; ++c)
                        {
                            texel[c] = static_cast<u8>(
                                (kInk[c] * alpha + texel[c] * (255u - alpha)) / 255u);
                        }
                    }
                }
                pen += r.advanceX;
            }
            return Status{};
        }
    };

    /// Registered by RegisterFontEditor (the domain's one composition entry point).
    inline void RegisterFontThumbnailGenerator(editor::ThumbnailService& service)
    {
        service.RegisterGenerator(MakeUnique<FontThumbnailGenerator>(editor::EditorRootAllocator()));
    }
}
