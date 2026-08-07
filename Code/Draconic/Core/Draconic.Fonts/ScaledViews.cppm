// Draconic::Fonts - draconic.fonts:scaled_views partition.
//
// SIZE VIEWS over a base font/atlas: everything screen-space (metrics, advances, kerning,
// glyph-quad geometry) scales by requestedPx / bakedPx while the atlas texels and UVs stay
// untouched. This is what makes ONE MSDF bake serve every UI size: the distance field
// resamples cleanly at any scale (the DF fragment shader anti-aliases in screen space), so
// a font service answers GetFont(family, 12) from a single 48px bake by handing out a
// scaled view instead of the raw 48px tables (which would draw 48px-geometry glyphs - the
// documented per-size-scaled-views gap that kept the UI off MSDF).
//
// Views BORROW the base objects (the service owns the base entries and guarantees they
// outlive the views); a CachedFont may own the view wrappers themselves.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.fonts:scaled_views;

import draconic.foundation;
import :types;
import :interfaces;
import :text_util;

using namespace draconic::foundation;

export namespace draconic::fonts
{
    class ScaledFontView final : public IFont
    {
    public:
        ScaledFontView(const IFont& base, f32 pixelHeight)
            : m_base(&base), m_pixelHeight(pixelHeight),
              m_scale(base.PixelHeight() > 0.0f ? pixelHeight / base.PixelHeight() : 1.0f)
        {
        }

        [[nodiscard]] StringView FamilyName() const override { return m_base->FamilyName(); }
        [[nodiscard]] f32 PixelHeight() const override { return m_pixelHeight; }
        [[nodiscard]] f32 Scale() const { return m_scale; }

        [[nodiscard]] FontMetrics Metrics() const override
        {
            const FontMetrics base = m_base->Metrics();
            return FontMetrics(base.ascent * m_scale, base.descent * m_scale,
                               base.lineGap * m_scale, m_pixelHeight, base.scale * m_scale);
        }

        [[nodiscard]] GlyphInfo GetGlyphInfo(i32 codepoint) const override
        {
            GlyphInfo info = m_base->GetGlyphInfo(codepoint);
            info.advanceWidth *= m_scale;
            info.leftSideBearing *= m_scale;
            info.boundingBox.x *= m_scale;
            info.boundingBox.y *= m_scale;
            info.boundingBox.width *= m_scale;
            info.boundingBox.height *= m_scale;
            return info;
        }

        [[nodiscard]] f32 GetKerning(i32 first, i32 second) const override
        {
            return m_base->GetKerning(first, second) * m_scale;
        }

        [[nodiscard]] bool HasGlyph(i32 codepoint) const override
        {
            return m_base->HasGlyph(codepoint);
        }

        [[nodiscard]] f32 MeasureString(StringView text) const override
        {
            return m_base->MeasureString(text) * m_scale;
        }

        [[nodiscard]] f32 MeasureString(StringView text,
                                        Array<GlyphPosition>& outPositions) const override
        {
            // Positions come out in the VIEW's space: advance/kerning walk with scaled infos.
            outPositions.Clear();
            f32 x = 0.0f;
            i32 previousCodepoint = 0;
            i32 index = 0;
            usize i = 0;
            while (i < text.Size())
            {
                const i32 codepoint = static_cast<i32>(DecodeCodepoint(text, i));
                const GlyphInfo info = GetGlyphInfo(codepoint);
                if (previousCodepoint != 0)
                {
                    x += GetKerning(previousCodepoint, codepoint);
                }
                outPositions.PushBack(
                    GlyphPosition(index, codepoint, x, 0, info.advanceWidth, info));
                x += info.advanceWidth;
                previousCodepoint = codepoint;
                ++index;
            }
            return x;
        }

    private:
        const IFont* m_base; // borrowed
        f32 m_pixelHeight;
        f32 m_scale;
    };

    class ScaledFontAtlasView final : public IFontAtlas
    {
    public:
        ScaledFontAtlasView(const IFontAtlas& base, f32 scale) : m_base(&base), m_scale(scale) {}

        // Texel-space facts pass through untouched (the texture IS the base atlas).
        [[nodiscard]] u32 Width() const override { return m_base->Width(); }
        [[nodiscard]] u32 Height() const override { return m_base->Height(); }
        [[nodiscard]] Span<const u8> PixelData() const override { return m_base->PixelData(); }
        [[nodiscard]] Float2 WhitePixelUV() const override { return m_base->WhitePixelUV(); }
        [[nodiscard]] AtlasMode Mode() const override { return m_base->Mode(); }
        [[nodiscard]] f32 DistanceFieldRange() const override
        {
            return m_base->DistanceFieldRange();
        }
        [[nodiscard]] bool Contains(i32 codepoint) const override
        {
            return m_base->Contains(codepoint);
        }

        // Screen-space fields of the region scale; texel rect stays (it indexes the atlas).
        [[nodiscard]] bool TryGetRegion(i32 codepoint, AtlasRegion& region) const override
        {
            if (!m_base->TryGetRegion(codepoint, region))
            {
                return false;
            }
            region.offsetX *= m_scale;
            region.offsetY *= m_scale;
            region.advanceX *= m_scale;
            return true;
        }

        [[nodiscard]] bool GetGlyphQuad(i32 codepoint, f32& cursorX, f32 cursorY,
                                        GlyphQuad& quad) const override
        {
            AtlasRegion region;
            if (!m_base->TryGetRegion(codepoint, region))
            {
                quad = GlyphQuad();
                return false;
            }
            // Advance-only regions (whitespace): step the cursor, nothing to draw.
            if (region.IsEmpty())
            {
                quad = GlyphQuad();
                cursorX += region.advanceX * m_scale;
                return false;
            }
            BuildQuad(region, cursorX, cursorY, quad);
            cursorX += region.advanceX * m_scale;
            return true;
        }

        [[nodiscard]] bool GetGlyphQuadAt(i32 codepoint, f32 x, f32 y,
                                          GlyphQuad& quad) const override
        {
            AtlasRegion region;
            if (!m_base->TryGetRegion(codepoint, region) || region.IsEmpty())
            {
                quad = GlyphQuad();
                return false;
            }
            BuildQuad(region, x, y, quad);
            return true;
        }

    private:
        void BuildQuad(const AtlasRegion& region, f32 x, f32 y, GlyphQuad& quad) const
        {
            const f32 qx0 = x + region.offsetX * m_scale;
            const f32 qy0 = y + region.offsetY * m_scale;
            const f32 qx1 = qx0 + static_cast<f32>(region.width) * m_scale;
            const f32 qy1 = qy0 + static_cast<f32>(region.height) * m_scale;
            f32 u0, v0, u1, v1;
            region.GetUVs(m_base->Width(), m_base->Height(), u0, v0, u1, v1);
            quad = GlyphQuad(qx0, qy0, qx1, qy1, u0, v0, u1, v1);
        }

        const IFontAtlas* m_base; // borrowed
        f32 m_scale;
    };
}
