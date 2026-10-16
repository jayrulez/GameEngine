// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor App - :font_atlas_cache partition
//
// Disk-backed IFontAtlasCache for the editor's MSDF UI fonts. The startup bake (msdfgen
// over two full families) dominates the launch-to-first-frame gap; this caches the baked
// DistanceFieldFontAtlas keyed by the font's bytes + the bake options, so every launch after the
// first loads the atlas in milliseconds. Coverage atlases and non-TTF fonts are declined
// (they are cheap and/or have no stable byte identity here).

module;
#include "Core/Prelude.h"

export module editor.app:font_atlas_cache;

import foundation.core;
import foundation.vfs;
import foundation.fonts;
import foundation.fonts.truetype;
import foundation.fonts.distancefield;

using namespace foundation::core;

namespace
{
    // Bump on any layout change - old cache files then miss and rebake. 2: distance-field atlases
    // are sized to their glyphs, so a cached full-size atlas is stale. 3: regions key by glyph
    // index, followed by the codepoint -> glyph map.
    constexpr u32 kCacheFormatVersion = 3;
    constexpr u32 kCacheMagic = 0x43414644u; // 'DFAC'

    struct CacheHeader
    {
        u32 magic = 0;
        u32 version = 0;
        u64 key = 0;
        f32 pixelRange = 0.0f;
        f32 whiteU = 0.0f;
        f32 whiteV = 0.0f;
        u32 width = 0;
        u32 height = 0;
        u32 regionCount = 0;
        u32 glyphMapCount = 0;
        u32 pixelBytes = 0;
    };

    struct CacheRegion
    {
        i32 glyph = 0;
        u16 x = 0, y = 0, width = 0, height = 0;
        f32 offsetX = 0.0f, offsetY = 0.0f, advanceX = 0.0f;
    };

    struct CacheGlyphMapping
    {
        i32 codepoint = 0;
        i32 glyph = 0;
    };
}

export namespace editor
{
    class FontAtlasDiskCache final : public foundation::fonts::IFontAtlasCache
    {
    public:
        // `directory` is created on first Store; a missing directory just means misses.
        FontAtlasDiskCache(IAllocator& allocator, StringView directory)
            : m_allocator(&allocator), m_directory(directory)
        {
        }

        [[nodiscard]] foundation::fonts::IFontAtlas*
        TryLoad(const foundation::fonts::IFont& font,
                const foundation::fonts::FontLoadOptions& options, IAllocator& allocator) override
        {
            u64 key = 0;
            if (!KeyFor(font, options, key))
            {
                return nullptr;
            }
            foundation::vfs::NativeFileSystem fs(m_directory.AsView(), *m_allocator);
            UniquePtr<IStream> stream = fs.Open(FileNameFor(key).AsView(), FileMode::Read);
            if (!stream)
            {
                return nullptr;
            }

            CacheHeader header;
            if (stream->Read(&header, sizeof(header)) != sizeof(header) ||
                header.magic != kCacheMagic || header.version != kCacheFormatVersion ||
                header.key != key || header.width == 0 || header.height == 0 ||
                header.pixelBytes != header.width * header.height * 4u)
            {
                return nullptr;
            }

            auto* atlas = allocator.New<foundation::fonts::DistanceFieldFontAtlas>();
            atlas->SetPixelRange(header.pixelRange);
            atlas->SetWhitePixelUV(header.whiteU, header.whiteV);
            for (u32 i = 0; i < header.regionCount; ++i)
            {
                CacheRegion r;
                if (stream->Read(&r, sizeof(r)) != sizeof(r))
                {
                    allocator.Delete(atlas);
                    return nullptr;
                }
                atlas->SetRegion(r.glyph,
                                 foundation::fonts::AtlasRegion(r.x, r.y, r.width, r.height,
                                                                r.offsetX, r.offsetY, r.advanceX));
            }
            for (u32 i = 0; i < header.glyphMapCount; ++i)
            {
                CacheGlyphMapping m;
                if (stream->Read(&m, sizeof(m)) != sizeof(m))
                {
                    allocator.Delete(atlas);
                    return nullptr;
                }
                atlas->SetGlyphFor(m.codepoint, m.glyph);
            }
            Array<u8> pixels(static_cast<usize>(header.pixelBytes));
            if (stream->Read(pixels.Data(), pixels.Size()) != pixels.Size())
            {
                allocator.Delete(atlas);
                return nullptr;
            }
            atlas->SetPixels(header.width, header.height, Move(pixels));
            return atlas;
        }

        void Store(const foundation::fonts::IFont& font,
                   const foundation::fonts::FontLoadOptions& options,
                   const foundation::fonts::IFontAtlas& atlas) override
        {
            u64 key = 0;
            if (!KeyFor(font, options, key) ||
                atlas.Mode() != foundation::fonts::AtlasMode::DistanceField)
            {
                return;
            }
            const auto& df = static_cast<const foundation::fonts::DistanceFieldFontAtlas&>(atlas);
            const Span<const u8> pixels = df.PixelData();
            if (pixels.IsEmpty())
            {
                return;
            }

            // Regions sorted by glyph and the map by codepoint: cache files are byte-stable for
            // identical bakes.
            Array<CacheRegion> regions;
            for (const auto& pair : df.Regions())
            {
                CacheRegion r;
                r.glyph = pair.key;
                r.x = pair.value.x;
                r.y = pair.value.y;
                r.width = pair.value.width;
                r.height = pair.value.height;
                r.offsetX = pair.value.offsetX;
                r.offsetY = pair.value.offsetY;
                r.advanceX = pair.value.advanceX;
                regions.PushBack(r);
            }
            for (usize i = 1; i < regions.Size(); ++i) // insertion sort; ~224 entries
            {
                CacheRegion item = regions[i];
                usize j = i;
                while (j > 0 && regions[j - 1].glyph > item.glyph)
                {
                    regions[j] = regions[j - 1];
                    --j;
                }
                regions[j] = item;
            }
            Array<CacheGlyphMapping> mappings;
            for (const auto& pair : df.CodepointGlyphs())
            {
                CacheGlyphMapping m;
                m.codepoint = pair.key;
                m.glyph = pair.value;
                mappings.PushBack(m);
            }
            for (usize i = 1; i < mappings.Size(); ++i)
            {
                CacheGlyphMapping item = mappings[i];
                usize j = i;
                while (j > 0 && mappings[j - 1].codepoint > item.codepoint)
                {
                    mappings[j] = mappings[j - 1];
                    --j;
                }
                mappings[j] = item;
            }

            CacheHeader header;
            header.magic = kCacheMagic;
            header.version = kCacheFormatVersion;
            header.key = key;
            header.pixelRange = df.DistanceFieldRange();
            const Float2 white = df.WhitePixelUV();
            header.whiteU = white.x;
            header.whiteV = white.y;
            header.width = df.Width();
            header.height = df.Height();
            header.regionCount = static_cast<u32>(regions.Size());
            header.glyphMapCount = static_cast<u32>(mappings.Size());
            header.pixelBytes = static_cast<u32>(pixels.Size());

            Array<u8> blob;
            blob.Reserve(sizeof(header) + regions.Size() * sizeof(CacheRegion) +
                         mappings.Size() * sizeof(CacheGlyphMapping) + pixels.Size());
            AppendBytes(blob, &header, sizeof(header));
            AppendBytes(blob, regions.Data(), regions.Size() * sizeof(CacheRegion));
            AppendBytes(blob, mappings.Data(), mappings.Size() * sizeof(CacheGlyphMapping));
            AppendBytes(blob, pixels.Data(), pixels.Size());

            (void)CreateDirectory(m_directory.AsView());
            foundation::vfs::NativeFileSystem fs(m_directory.AsView(), *m_allocator);
            if (foundation::vfs::IWritableFileSystem* writable = fs.AsWritable())
            {
                (void)writable->Save(FileNameFor(key).AsView(),
                                     Span<const byte>{reinterpret_cast<const byte*>(blob.Data()),
                                                      blob.Size()});
            }
        }

    private:
        // Key = font bytes + every option that shapes the bake + the format version.
        // Only TTF distance-field bakes are cacheable (RawData is the byte identity).
        [[nodiscard]] static bool KeyFor(const foundation::fonts::IFont& font,
                                         const foundation::fonts::FontLoadOptions& options,
                                         u64& outKey)
        {
            if (options.atlasMode != foundation::fonts::AtlasMode::DistanceField ||
                font.BackendTypeId() != foundation::fonts::kTrueTypeFontTypeId)
            {
                return false;
            }
            const auto& ttf = static_cast<const foundation::fonts::TrueTypeFont&>(font);
            u64 key = HashBytes(ttf.RawData(), ttf.RawDataSize());
            const auto mix = [&key](u64 v)
            { key = HashInteger(key ^ HashInteger(v)); };
            u32 heightBits = 0;
            MemCopy(&heightBits, &options.pixelHeight, sizeof(heightBits));
            mix(heightBits);
            mix(static_cast<u64>(static_cast<u32>(options.firstCodepoint)));
            mix(static_cast<u64>(static_cast<u32>(options.lastCodepoint)));
            mix(options.atlasWidth);
            mix(options.atlasHeight);
            mix(options.padding);
            mix(kCacheFormatVersion);
            outKey = key;
            return true;
        }

        [[nodiscard]] static String FileNameFor(u64 key)
        {
            static const char8_t* hex = u8"0123456789abcdef";
            String name;
            for (i32 shift = 60; shift >= 0; shift -= 4)
            {
                name.PushBack(hex[(key >> shift) & 0xF]);
            }
            name.Append(u8".dfatlas");
            return name;
        }

        static void AppendBytes(Array<u8>& blob, const void* data, usize size)
        {
            const usize at = blob.Size();
            blob.Resize(at + size);
            MemCopy(blob.Data() + at, data, size);
        }

        IAllocator* m_allocator;
        String m_directory;
    };
} // namespace editor
