// Draconic::FontsTTF - draconic.fonts.ttf:atlas partition
//
// IFontAtlas produced by stb_truetype's packed-font API. Holds the packed
// 8-bit coverage buffer + per-codepoint stbtt_packedchar records and maps
// codepoints to atlas regions / glyph quads. Ported from
// Sedulous.Fonts.TTF/TrueTypeFontAtlas.bf.

module;
#include "Draconic.Foundation/Prelude.h"
#include "stb_truetype.h"

export module draconic.fonts.ttf:atlas;

import draconic.foundation;
import draconic.fonts;
import :font;

using namespace draconic::foundation;

export namespace draconic::fonts
{
    class TrueTypeFontAtlas final : public IFontAtlas
    {
    public:
        TrueTypeFontAtlas() = default;
        ~TrueTypeFontAtlas() override = default;

        TrueTypeFontAtlas(const TrueTypeFontAtlas&) = delete;
        TrueTypeFontAtlas& operator=(const TrueTypeFontAtlas&) = delete;

        // Pack `font` into a new atlas per `options`. Returns Success or a
        // specific failure.
        [[nodiscard]] FontLoadResult Create(const TrueTypeFont& font, FontLoadOptions options)
        {
            m_width = options.atlasWidth;
            m_height = options.atlasHeight;
            m_firstCodepoint = options.firstCodepoint;
            m_lastCodepoint = options.lastCodepoint;
            m_numChars = m_lastCodepoint - m_firstCodepoint + 1;

            m_pixelData.Resize(static_cast<usize>(m_width) * m_height);
            m_packedChars.Resize(static_cast<usize>(m_numChars));

            stbtt_pack_context packContext{};
            if (stbtt_PackBegin(&packContext, m_pixelData.Data(), static_cast<int>(m_width),
                                static_cast<int>(m_height), 0, static_cast<int>(options.padding),
                                nullptr) == 0)
                return FontLoadResult::AtlasPackingFailed;

            stbtt_PackSetOversampling(&packContext, options.oversampleX, options.oversampleY);

            if (stbtt_PackFontRange(&packContext, font.RawData(), 0, options.pixelHeight,
                                    m_firstCodepoint, m_numChars, m_packedChars.Data()) == 0)
            {
                stbtt_PackEnd(&packContext);
                return FontLoadResult::AtlasPackingFailed;
            }
            stbtt_PackEnd(&packContext);

            // Solid white 2x2 cell near the bottom-right for drawing lines/rects.
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

        [[nodiscard]] bool TryGetRegion(i32 codepoint, AtlasRegion& region) const override
        {
            region = AtlasRegion();
            if (codepoint < m_firstCodepoint || codepoint > m_lastCodepoint)
                return false;

            const stbtt_packedchar& pc =
                m_packedChars[static_cast<usize>(codepoint - m_firstCodepoint)];
            if (pc.x1 <= pc.x0 || pc.y1 <= pc.y0)
                return false;

            region.x = pc.x0;
            region.y = pc.y0;
            region.width = static_cast<u16>(pc.x1 - pc.x0);
            region.height = static_cast<u16>(pc.y1 - pc.y0);
            region.offsetX = pc.xoff;
            region.offsetY = pc.yoff;
            region.advanceX = pc.xadvance;
            return true;
        }

        [[nodiscard]] bool GetGlyphQuad(i32 codepoint, f32& cursorX, f32 cursorY,
                                        GlyphQuad& quad) const override
        {
            quad = GlyphQuad();
            if (codepoint < m_firstCodepoint || codepoint > m_lastCodepoint)
                return false;

            const int index = codepoint - m_firstCodepoint;
            stbtt_aligned_quad q{};
            f32 xpos = cursorX;
            f32 ypos = cursorY;
            stbtt_GetPackedQuad(m_packedChars.Data(), static_cast<int>(m_width),
                                static_cast<int>(m_height), index, &xpos, &ypos, &q,
                                0); // 0 = don't align to integer
            cursorX = xpos;
            quad = GlyphQuad(q.x0, q.y0, q.x1, q.y1, q.s0, q.t0, q.s1, q.t1);
            return true;
        }

        [[nodiscard]] bool GetGlyphQuadAt(i32 codepoint, f32 x, f32 y,
                                          GlyphQuad& quad) const override
        {
            quad = GlyphQuad();
            if (codepoint < m_firstCodepoint || codepoint > m_lastCodepoint)
                return false;

            const int index = codepoint - m_firstCodepoint;
            stbtt_aligned_quad q{};
            f32 xpos = x;
            f32 ypos = y;
            stbtt_GetPackedQuad(m_packedChars.Data(), static_cast<int>(m_width),
                                static_cast<int>(m_height), index, &xpos, &ypos, &q, 0);
            quad = GlyphQuad(q.x0, q.y0, q.x1, q.y1, q.s0, q.t0, q.s1, q.t1);
            return true;
        }

        [[nodiscard]] bool Contains(i32 codepoint) const override
        {
            if (codepoint < m_firstCodepoint || codepoint > m_lastCodepoint)
                return false;
            const stbtt_packedchar& pc =
                m_packedChars[static_cast<usize>(codepoint - m_firstCodepoint)];
            return pc.x1 > pc.x0 && pc.y1 > pc.y0;
        }

    private:
        u32 m_width = 0;
        u32 m_height = 0;
        Array<u8> m_pixelData;
        Array<stbtt_packedchar> m_packedChars;
        i32 m_firstCodepoint = 0;
        i32 m_lastCodepoint = 0;
        i32 m_numChars = 0;
        f32 m_whitePixelU = 0;
        f32 m_whitePixelV = 0;
    };
}
