// Draconic::FontsDF - draconic.fonts.distancefield:atlas partition
//
// IFontAtlas for distance-field (MSDF) atlases: stores RGBA8 linear pixel data
// plus per-glyph AtlasRegion entries. Pure data type with no msdfgen dependency;
// loadable from baked assets at runtime.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.fonts.distancefield:atlas;

import draconic.foundation;
import draconic.fonts;

using namespace draconic::foundation;

export namespace draconic::fonts
{
    class DFFontAtlas final : public IFontAtlas
    {
    public:
        DFFontAtlas() = default;

        void SetPixels(u32 width, u32 height, Array<u8>&& pixels)
        {
            m_width = width;
            m_height = height;
            m_pixels = Move(pixels);
        }

        void SetRegion(i32 codepoint, AtlasRegion region)
        {
            m_regions.InsertOrAssign(codepoint, region);
        }
        void SetWhitePixelUV(f32 u, f32 v)
        {
            m_whitePixelU = u;
            m_whitePixelV = v;
        }
        void SetPixelRange(f32 pxRange) { m_pxRange = pxRange; }

        // --- IFontAtlas -------------------------------------------------------
        [[nodiscard]] u32 Width() const override { return m_width; }
        [[nodiscard]] u32 Height() const override { return m_height; }

        [[nodiscard]] Span<const u8> PixelData() const override
        {
            return m_pixels.Size() != 0 ? Span<const u8>(m_pixels.Data(), m_pixels.Size())
                                        : Span<const u8>();
        }

        [[nodiscard]] bool TryGetRegion(i32 codepoint, AtlasRegion& region) const override
        {
            if (const AtlasRegion* r = m_regions.Find(codepoint))
            {
                region = *r;
                return true;
            }
            return false;
        }

        [[nodiscard]] bool Contains(i32 codepoint) const override
        {
            return m_regions.Contains(codepoint);
        }

        [[nodiscard]] Float2 WhitePixelUV() const override
        {
            return Float2(m_whitePixelU, m_whitePixelV);
        }

        [[nodiscard]] bool GetGlyphQuad(i32 codepoint, f32& cursorX, f32 cursorY,
                                        GlyphQuad& quad) const override
        {
            quad = GlyphQuad();
            const AtlasRegion* region = m_regions.Find(codepoint);
            if (region == nullptr)
                return false;
            return BuildQuad(*region, cursorX, cursorY, true, cursorX, quad);
        }

        [[nodiscard]] bool GetGlyphQuadAt(i32 codepoint, f32 x, f32 y,
                                          GlyphQuad& quad) const override
        {
            quad = GlyphQuad();
            const AtlasRegion* region = m_regions.Find(codepoint);
            if (region == nullptr)
                return false;
            f32 dummy = x;
            return BuildQuad(*region, x, y, false, dummy, quad);
        }

        [[nodiscard]] AtlasMode Mode() const override { return AtlasMode::DistanceField; }
        [[nodiscard]] f32 DistanceFieldRange() const override { return m_pxRange; }

        [[nodiscard]] const HashMap<i32, AtlasRegion>& Regions() const { return m_regions; }

    private:
        bool BuildQuad(const AtlasRegion& region, f32 x, f32 y, bool advance, f32& cursorX,
                       GlyphQuad& quad) const
        {
            // Advance-only regions (whitespace): step the cursor, nothing to draw.
            if (region.IsEmpty())
            {
                if (advance)
                    cursorX = x + region.advanceX;
                return false;
            }

            const f32 invW = 1.0f / static_cast<f32>(m_width);
            const f32 invH = 1.0f / static_cast<f32>(m_height);

            const f32 qx0 = x + region.offsetX;
            const f32 qy0 = y + region.offsetY;
            const f32 qx1 = qx0 + static_cast<f32>(region.width);
            const f32 qy1 = qy0 + static_cast<f32>(region.height);

            const f32 u0 = static_cast<f32>(region.x) * invW;
            const f32 v0 = static_cast<f32>(region.y) * invH;
            const f32 u1 = static_cast<f32>(region.x + region.width) * invW;
            const f32 v1 = static_cast<f32>(region.y + region.height) * invH;

            quad = GlyphQuad(qx0, qy0, qx1, qy1, u0, v0, u1, v1);

            if (advance)
                cursorX = x + region.advanceX;
            return true;
        }

        u32 m_width = 0;
        u32 m_height = 0;
        f32 m_pxRange = 4.0f;
        Array<u8> m_pixels; // RGBA8 linear, size = w * h * 4
        HashMap<i32, AtlasRegion> m_regions;
        f32 m_whitePixelU = 0;
        f32 m_whitePixelV = 0;
    };
}
