// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Fonts.TrueType - foundation.fonts.truetype:atlas partition
//
// IFontAtlas produced by stb_truetype's packer, keyed by glyph index: the glyphs a codepoint range
// maps to (and any further glyphs asked for, such as what a shaper's substitutions reach), packed
// through EngineStbPackGlyphs, with the range's codepoint -> glyph map for unshaped text.

module;
#include "Core/Prelude.h"
#include "stb_truetype.h"
#include "stb_truetype_glyphs.h"

export module foundation.fonts.truetype:atlas;

import foundation.core;
import foundation.fonts;
import :font;

using namespace foundation::core;

export namespace foundation::fonts
{
    class TrueTypeFontAtlas final : public IFontAtlas
    {
    public:
        TrueTypeFontAtlas() = default;
        ~TrueTypeFontAtlas() override = default;

        TrueTypeFontAtlas(const TrueTypeFontAtlas&) = delete;
        TrueTypeFontAtlas& operator=(const TrueTypeFontAtlas&) = delete;

        // Pack `font` into a new atlas per `options`: the glyphs of its codepoint range, plus
        // `extraGlyphs` (glyph indices no codepoint reaches). Returns Success or a specific failure.
        [[nodiscard]] FontLoadResult Create(const TrueTypeFont& font, FontLoadOptions options,
                                            Span<const i32> extraGlyphs = {})
        {
            m_width = options.atlasWidth;
            m_height = options.atlasHeight;
            m_pixelData.Resize(static_cast<usize>(m_width) * m_height);

            stbtt_fontinfo info{};
            const int offset = stbtt_GetFontOffsetForIndex(font.RawData(), 0);
            if (offset < 0 || stbtt_InitFont(&info, font.RawData(), offset) == 0)
                return FontLoadResult::CorruptedData;

            // The range's glyphs (a codepoint the font lacks maps to none), then the extras, each once.
            for (i32 cp = options.firstCodepoint; cp <= options.lastCodepoint; ++cp)
            {
                const i32 glyph = stbtt_FindGlyphIndex(&info, cp);
                if (glyph <= 0)
                    continue;
                m_cmap.InsertOrAssign(cp, glyph);
                AddGlyph(glyph);
            }
            for (const i32 glyph : extraGlyphs)
            {
                if (glyph > 0 && glyph < info.numGlyphs)
                    AddGlyph(glyph);
            }
            m_packedChars.Resize(m_glyphs.Size());

            stbtt_pack_context packContext{};
            if (stbtt_PackBegin(&packContext, m_pixelData.Data(), static_cast<int>(m_width),
                                static_cast<int>(m_height), 0, static_cast<int>(options.padding),
                                nullptr) == 0)
                return FontLoadResult::AtlasPackingFailed;

            stbtt_PackSetOversampling(&packContext, options.oversampleX, options.oversampleY);

            if (EngineStbPackGlyphs(&packContext, &info, options.pixelHeight, m_glyphs.Data(),
                                    static_cast<int>(m_glyphs.Size()), m_packedChars.Data()) == 0)
            {
                stbtt_PackEnd(&packContext);
                return FontLoadResult::AtlasPackingFailed;
            }
            stbtt_PackEnd(&packContext);

            // Solid white 2x2 cell near the bottom-right for drawing lines and rects.
            const u32 whiteX = m_width - 2;
            const u32 whiteY = m_height - 2;
            m_pixelData[whiteY * m_width + whiteX] = 255;
            m_pixelData[whiteY * m_width + whiteX + 1] = 255;
            m_pixelData[(whiteY + 1) * m_width + whiteX] = 255;
            m_pixelData[(whiteY + 1) * m_width + whiteX + 1] = 255;
            m_whitePixelU = (whiteX + 0.5f) / static_cast<f32>(m_width);
            m_whitePixelV = (whiteY + 0.5f) / static_cast<f32>(m_height);

            return FontLoadResult::Success;
        }

        // --- IFontAtlas ----------------------------------------------------
        [[nodiscard]] u32 Width() const override { return m_width; }
        [[nodiscard]] u32 Height() const override { return m_height; }
        [[nodiscard]] Span<const u8> PixelData() const override
        {
            return Span<const u8>(m_pixelData.Data(), m_pixelData.Size());
        }
        [[nodiscard]] Float2 WhitePixelUV() const override
        {
            return Float2(m_whitePixelU, m_whitePixelV);
        }

        [[nodiscard]] i32 GlyphFor(i32 codepoint) const override
        {
            const i32* glyph = m_cmap.Find(codepoint);
            return glyph != nullptr ? *glyph : -1;
        }

        [[nodiscard]] bool TryGetRegion(i32 glyphId, AtlasRegion& region) const override
        {
            region = AtlasRegion();
            const stbtt_packedchar* pc = Packed(glyphId);
            if (pc == nullptr || pc->x1 <= pc->x0 || pc->y1 <= pc->y0)
                return false;

            region.x = pc->x0;
            region.y = pc->y0;
            region.width = static_cast<u16>(pc->x1 - pc->x0);
            region.height = static_cast<u16>(pc->y1 - pc->y0);
            region.offsetX = pc->xoff;
            region.offsetY = pc->yoff;
            region.advanceX = pc->xadvance;
            return true;
        }

        [[nodiscard]] bool GetGlyphQuad(i32 glyphId, f32& cursorX, f32 cursorY,
                                        GlyphQuad& quad) const override
        {
            quad = GlyphQuad();
            const usize* index = m_glyphIndex.Find(glyphId);
            if (index == nullptr)
                return false;

            stbtt_aligned_quad q{};
            f32 xpos = cursorX;
            f32 ypos = cursorY;
            stbtt_GetPackedQuad(m_packedChars.Data(), static_cast<int>(m_width),
                                static_cast<int>(m_height), static_cast<int>(*index), &xpos, &ypos, &q,
                                0); // 0 = don't align to integer
            cursorX = xpos;
            quad = GlyphQuad(q.x0, q.y0, q.x1, q.y1, q.s0, q.t0, q.s1, q.t1);
            return true;
        }

        [[nodiscard]] bool GetGlyphQuadAt(i32 glyphId, f32 x, f32 y,
                                          GlyphQuad& quad) const override
        {
            quad = GlyphQuad();
            const usize* index = m_glyphIndex.Find(glyphId);
            if (index == nullptr)
                return false;

            stbtt_aligned_quad q{};
            f32 xpos = x;
            f32 ypos = y;
            stbtt_GetPackedQuad(m_packedChars.Data(), static_cast<int>(m_width),
                                static_cast<int>(m_height), static_cast<int>(*index), &xpos, &ypos, &q, 0);
            quad = GlyphQuad(q.x0, q.y0, q.x1, q.y1, q.s0, q.t0, q.s1, q.t1);
            return true;
        }

        [[nodiscard]] bool Contains(i32 glyphId) const override
        {
            const stbtt_packedchar* pc = Packed(glyphId);
            return pc != nullptr && pc->x1 > pc->x0 && pc->y1 > pc->y0;
        }

        /// The glyphs packed, in packing order, and the codepoint -> glyph map (for bakers that copy
        /// this atlas into their own).
        [[nodiscard]] Span<const i32> Glyphs() const { return Span<const i32>(m_glyphs.Data(), m_glyphs.Size()); }
        [[nodiscard]] const HashMap<i32, i32>& CodepointGlyphs() const { return m_cmap; }

    private:
        void AddGlyph(i32 glyph)
        {
            if (m_glyphIndex.Find(glyph) != nullptr)
                return;
            m_glyphIndex.InsertOrAssign(glyph, m_glyphs.Size());
            m_glyphs.PushBack(glyph);
        }

        [[nodiscard]] const stbtt_packedchar* Packed(i32 glyphId) const
        {
            const usize* index = m_glyphIndex.Find(glyphId);
            return index != nullptr ? &m_packedChars[*index] : nullptr;
        }

        u32 m_width = 0;
        u32 m_height = 0;
        Array<u8> m_pixelData;
        Array<i32> m_glyphs;                 // glyph indices, in packing order
        Array<stbtt_packedchar> m_packedChars; // parallel to m_glyphs
        HashMap<i32, usize> m_glyphIndex;    // glyph index -> its slot
        HashMap<i32, i32> m_cmap;            // codepoint -> glyph index (the range's)
        f32 m_whitePixelU = 0;
        f32 m_whitePixelV = 0;
    };
}
