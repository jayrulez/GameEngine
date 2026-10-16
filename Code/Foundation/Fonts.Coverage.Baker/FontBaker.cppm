// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Fonts.Coverage.Baker - the `foundation.fonts.coverage.baker` module.
//
// Tooling-time baking: turns TTF/OTF/TTC bytes into pre-rasterized COVERAGE
// atlases - BakedFont + BakedFontAtlas (Fonts.Coverage), the Alpha8 counterpart of
// what Fonts.DistanceField.Baker produces. The packaged game loads those and
// never re-invokes the rasterizer. Ported from Sedulous.Fonts.Importer; renamed
// 2026-09-05 (the font family is named by atlas pixel format + role: Fonts.Coverage /
// Fonts.DistanceField are the runtime products, Fonts.Coverage.Baker /
// Fonts.DistanceField.Baker the tooling bakers). "Importer" here would mean the
// pipeline's IFileImporter, and this is not one: no Asset, no cook context, no
// content DB. It stays in
// Foundation (not Pipeline) because pipeline-free consumers bake with it - the
// editor's live font preview and thumbnails, and the VG pixel-probe tests.

module;
#include "Core/Prelude.h"

export module foundation.fonts.coverage.baker;

import foundation.core;
import foundation.fonts;
import foundation.fonts.coverage;
import foundation.fonts.truetype;

using namespace foundation::core;

export namespace foundation::fonts
{
    // Owns a (BakedFont, BakedFontAtlas) pair produced by an import. The
    // destructor frees both unless TakeOwnership() transfers them out.
    class BakedFontData
    {
    public:
        // `allocator` is the one font/atlas were allocated from (the Bake caller's
        // decision) - the pair frees through it.
        BakedFontData(IAllocator& allocator, BakedFont* font, BakedFontAtlas* atlas)
            : font(font), atlas(atlas), m_allocator(&allocator)
        {
        }

        ~BakedFontData()
        {
            if (font != nullptr)
                m_allocator->Delete(font);
            if (atlas != nullptr)
                m_allocator->Delete(atlas);
        }

        BakedFontData(const BakedFontData&) = delete;
        BakedFontData& operator=(const BakedFontData&) = delete;

        // Release ownership of both objects (nulling our fields) so a caller
        // can hand them to a FontResource without the destructor freeing them.
        void TakeOwnership(BakedFont*& outFont, BakedFontAtlas*& outAtlas)
        {
            outFont = font;
            outAtlas = atlas;
            font = nullptr;
            atlas = nullptr;
        }

        BakedFont* font = nullptr;
        BakedFontAtlas* atlas = nullptr;

    private:
        IAllocator* m_allocator;
    };

    class FontBaker
    {
    public:
        // Bake a font from raw TTF/OTF/TTC bytes. Caller owns the returned
        // BakedFontData (delete it or call TakeOwnership). Errors cleanly on
        // bad input.
        // The returned BakedFontData (and the font/atlas it owns) is allocated from
        // `allocator`; the caller frees it through the same allocator.
        [[nodiscard]] static Result<BakedFontData*, FontLoadResult>
        Bake(Span<const u8> data, FontLoadOptions options, IAllocator& allocator)
        {
            // Reuse TrueTypeFont as the parser; it owns its byte buffer, so
            // copy the input into a fresh array.
            Array<u8> bytesCopy;
            bytesCopy.Resize(data.Size());
            if (data.Size() != 0)
                MemCopy(bytesCopy.Data(), data.Data(), data.Size());

            TrueTypeFont* ttFont = allocator.New<TrueTypeFont>();
            const FontLoadResult initResult =
                ttFont->Initialize(Move(bytesCopy), options.pixelHeight);
            if (initResult != FontLoadResult::Success)
            {
                allocator.Delete(ttFont);
                return Err(initResult);
            }

            // The range's glyphs, and what the registered shaper's substitutions reach from them.
            const Array<i32> reachable = ReachableGlyphs(*ttFont, options, allocator);
            TrueTypeFontAtlas* ttAtlas = allocator.New<TrueTypeFontAtlas>();
            const FontLoadResult atlasResult =
                ttAtlas->Create(*ttFont, options, Span<const i32>(reachable.Data(), reachable.Size()));
            if (atlasResult != FontLoadResult::Success)
            {
                allocator.Delete(ttAtlas);
                allocator.Delete(ttFont);
                return Err(atlasResult);
            }

            // Build the baked font from the parsed metrics.
            BakedFont* baked = allocator.New<BakedFont>();
            baked->SetFamilyName(ttFont->FamilyName());
            baked->SetPixelHeight(ttFont->PixelHeight());
            baked->SetMetrics(ttFont->Metrics());

            // Copy the rasterized pixels into the baked atlas.
            BakedFontAtlas* bakedAtlas = allocator.New<BakedFontAtlas>();
            const u32 atlasW = ttAtlas->Width();
            const u32 atlasH = ttAtlas->Height();
            const Span<const u8> srcPixels = ttAtlas->PixelData();
            Array<u8> pixelCopy;
            pixelCopy.Resize(static_cast<usize>(atlasW) * atlasH);
            if (srcPixels.Size() > 0)
                MemCopy(pixelCopy.Data(), srcPixels.Data(),
                        Min(srcPixels.Size(), pixelCopy.Size()));
            bakedAtlas->SetPixels(atlasW, atlasH, Move(pixelCopy));

            const Float2 white = ttAtlas->WhitePixelUV();
            bakedAtlas->SetWhitePixelUV(white.x, white.y);
            bakedAtlas->SetOversample(static_cast<f32>(options.oversampleX),
                                      static_cast<f32>(options.oversampleY));

            // Every codepoint the range maps to keeps its glyph info and its glyph (a space too: it
            // draws nothing but advances), and every glyph that packed its region, by glyph index; a
            // mapped glyph with nothing to draw gets an advance-only region.
            for (const auto& pair : ttAtlas->CodepointGlyphs())
            {
                const GlyphInfo info = ttFont->GetGlyphInfo(pair.key);
                baked->SetGlyph(pair.key, info);
                bakedAtlas->SetGlyphFor(pair.key, pair.value);
                AtlasRegion region;
                if (!ttAtlas->TryGetRegion(pair.value, region))
                    bakedAtlas->SetRegion(pair.value, AtlasRegion(0, 0, 0, 0, 0.0f, 0.0f, info.advanceWidth));
            }
            for (const i32 glyph : ttAtlas->Glyphs())
            {
                AtlasRegion region;
                if (ttAtlas->TryGetRegion(glyph, region))
                    bakedAtlas->SetRegion(glyph, region);
            }

            // Capture non-zero kerning pairs within the range (quadratic but
            // trivial per pair; ~9k lookups for the default ASCII range).
            for (i32 a = options.firstCodepoint; a <= options.lastCodepoint; ++a)
            {
                if (!ttAtlas->Contains(ttAtlas->GlyphFor(a)))
                    continue;
                for (i32 b = options.firstCodepoint; b <= options.lastCodepoint; ++b)
                {
                    if (!ttAtlas->Contains(ttAtlas->GlyphFor(b)))
                        continue;
                    const f32 adj = ttFont->GetKerning(a, b);
                    if (adj != 0)
                        baked->SetKerning(a, b, adj);
                }
            }

            allocator.Delete(ttAtlas);
            allocator.Delete(ttFont);

            return allocator.New<BakedFontData>(allocator, baked, bakedAtlas);
        }
    };
}
