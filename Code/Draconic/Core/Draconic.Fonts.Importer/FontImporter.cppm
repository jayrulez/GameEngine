// Draconic::FontsImporter - the `draconic.fonts.importer` module.
//
// Editor/build-time baking: turns TTF/OTF/TTC bytes into pre-rasterized
// BakedFont + BakedFontAtlas objects (the shipped game then loads those and
// never re-invokes the rasterizer). Ported from Sedulous.Fonts.Importer -
// its own library, matching Sedulous.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.fonts.importer;

import draconic.foundation;
import draconic.fonts;
import draconic.fonts.baked;
import draconic.fonts.ttf;

using namespace draconic::foundation;

export namespace draconic::fonts
{
    // Owns a (BakedFont, BakedFontAtlas) pair produced by an import. The
    // destructor frees both unless TakeOwnership() transfers them out.
    class BakedFontData
    {
    public:
        BakedFontData(BakedFont* font, BakedFontAtlas* atlas) : font(font), atlas(atlas) {}

        ~BakedFontData()
        {
            if (font != nullptr)
                DefaultAllocator().Delete(font);
            if (atlas != nullptr)
                DefaultAllocator().Delete(atlas);
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
    };

    class FontImporter
    {
    public:
        // Bake a font from raw TTF/OTF/TTC bytes. Caller owns the returned
        // BakedFontData (delete it or call TakeOwnership). Errors cleanly on
        // bad input.
        [[nodiscard]] static Result<BakedFontData*, FontLoadResult>
        Bake(Span<const u8> data, FontLoadOptions options = FontLoadOptions::Default())
        {
            // Reuse TrueTypeFont as the parser; it owns its byte buffer, so
            // copy the input into a fresh array.
            Array<u8> bytesCopy;
            bytesCopy.Resize(data.Size());
            if (data.Size() != 0)
                MemCopy(bytesCopy.Data(), data.Data(), data.Size());

            TrueTypeFont* ttFont = DefaultAllocator().New<TrueTypeFont>();
            const FontLoadResult initResult =
                ttFont->Initialize(Move(bytesCopy), options.pixelHeight);
            if (initResult != FontLoadResult::Success)
            {
                DefaultAllocator().Delete(ttFont);
                return Err(initResult);
            }

            TrueTypeFontAtlas* ttAtlas = DefaultAllocator().New<TrueTypeFontAtlas>();
            const FontLoadResult atlasResult = ttAtlas->Create(*ttFont, options);
            if (atlasResult != FontLoadResult::Success)
            {
                DefaultAllocator().Delete(ttAtlas);
                DefaultAllocator().Delete(ttFont);
                return Err(atlasResult);
            }

            // Build the baked font from the parsed metrics.
            BakedFont* baked = DefaultAllocator().New<BakedFont>();
            baked->SetFamilyName(ttFont->FamilyName());
            baked->SetPixelHeight(ttFont->PixelHeight());
            baked->SetMetrics(ttFont->Metrics());

            // Copy the rasterized pixels into the baked atlas.
            BakedFontAtlas* bakedAtlas = DefaultAllocator().New<BakedFontAtlas>();
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

            // Copy GlyphInfo + AtlasRegion for every codepoint that packed.
            for (i32 cp = options.firstCodepoint; cp <= options.lastCodepoint; ++cp)
            {
                AtlasRegion region;
                if (!ttAtlas->TryGetRegion(cp, region))
                    continue;
                baked->SetGlyph(cp, ttFont->GetGlyphInfo(cp));
                bakedAtlas->SetRegion(cp, region);
            }

            // Capture non-zero kerning pairs within the range (quadratic but
            // trivial per pair; ~9k lookups for the default ASCII range).
            for (i32 a = options.firstCodepoint; a <= options.lastCodepoint; ++a)
            {
                if (!ttAtlas->Contains(a))
                    continue;
                for (i32 b = options.firstCodepoint; b <= options.lastCodepoint; ++b)
                {
                    if (!ttAtlas->Contains(b))
                        continue;
                    const f32 adj = ttFont->GetKerning(a, b);
                    if (adj != 0)
                        baked->SetKerning(a, b, adj);
                }
            }

            DefaultAllocator().Delete(ttAtlas);
            DefaultAllocator().Delete(ttFont);

            return DefaultAllocator().New<BakedFontData>(baked, bakedAtlas);
        }
    };
}
