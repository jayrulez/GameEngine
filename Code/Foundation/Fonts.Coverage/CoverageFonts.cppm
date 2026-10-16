// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Fonts.Coverage - the `foundation.fonts.coverage` module.
//
// Pre-baked IFont / IFontAtlas implementations with no rasterizer dependency:
// a packaged game gets every glyph + region from disk (resource deserialization)
// and never touch stb_truetype. Ported from Sedulous.Fonts.Baked
// (BakedFont.bf, BakedFontAtlas.bf) - its own library, matching Sedulous.

module;
#include "Core/Prelude.h"

export module foundation.fonts.coverage;

import foundation.core;
import foundation.fonts;

using namespace foundation::core;

export namespace foundation::fonts
{
    // IFont backed by pre-baked glyph + kerning tables.
    class BakedFont final : public IFont
    {
    public:
        BakedFont() = default;

        // Clears all glyph + kerning tables and metadata in place so the same
        // instance can be re-populated from disk on hot-reload without
        // invalidating outside references.
        void ClearForReload()
        {
            m_glyphs.Clear();
            m_kerning.Clear();
            m_familyName.Clear();
            m_pixelHeight = 0;
            m_metrics = FontMetrics::Default();
        }

        void SetFamilyName(StringView name) { m_familyName = String(name); }
        void SetMetrics(FontMetrics m) { m_metrics = m; }
        void SetPixelHeight(f32 h) { m_pixelHeight = h; }

        void SetGlyph(i32 codepoint, GlyphInfo info) { m_glyphs.InsertOrAssign(codepoint, info); }

        void SetKerning(i32 first, i32 second, f32 adjustment)
        {
            m_kerning.InsertOrAssign(PackKey(first, second), adjustment);
        }

        // --- IFont ---------------------------------------------------------
        [[nodiscard]] StringView FamilyName() const override { return m_familyName; }
        [[nodiscard]] FontMetrics Metrics() const override { return m_metrics; }
        [[nodiscard]] f32 PixelHeight() const override { return m_pixelHeight; }

        [[nodiscard]] GlyphInfo GetGlyphInfo(i32 codepoint) const override
        {
            if (const GlyphInfo* info = m_glyphs.Find(codepoint))
                return *info;
            return GlyphInfo();
        }

        [[nodiscard]] f32 GetKerning(i32 firstCodepoint, i32 secondCodepoint) const override
        {
            if (const f32* adj = m_kerning.Find(PackKey(firstCodepoint, secondCodepoint)))
                return *adj;
            return 0;
        }

        [[nodiscard]] bool HasGlyph(i32 codepoint) const override
        {
            return m_glyphs.Contains(codepoint);
        }

        [[nodiscard]] f32 MeasureString(StringView text) const override
        {
            f32 width = 0;
            i32 prevCodepoint = 0;
            usize i = 0;
            while (i < text.Size())
            {
                const i32 codepoint = static_cast<i32>(DecodeCodepoint(text, i));
                const GlyphInfo info = GetGlyphInfo(codepoint);
                if (prevCodepoint != 0)
                    width += GetKerning(prevCodepoint, codepoint);
                width += info.advanceWidth;
                prevCodepoint = codepoint;
            }
            return width;
        }

        [[nodiscard]] f32 MeasureString(StringView text,
                                        Array<GlyphPosition>& outPositions) const override
        {
            f32 x = 0;
            i32 prevCodepoint = 0;
            i32 index = 0;
            usize i = 0;
            outPositions.Clear();
            while (i < text.Size())
            {
                const i32 codepoint = static_cast<i32>(DecodeCodepoint(text, i));
                const GlyphInfo info = GetGlyphInfo(codepoint);
                if (prevCodepoint != 0)
                    x += GetKerning(prevCodepoint, codepoint);
                outPositions.PushBack(
                    GlyphPosition(index, codepoint, x, 0, info.advanceWidth, info));
                x += info.advanceWidth;
                prevCodepoint = codepoint;
                ++index;
            }
            return x;
        }

        // Read access for tooling that wants to iterate the baked tables.
        [[nodiscard]] const HashMap<i32, GlyphInfo>& Glyphs() const { return m_glyphs; }
        [[nodiscard]] const HashMap<i64, f32>& Kerning() const { return m_kerning; }

    private:
        // Kerning pairs packed as (first << 32) | (second & 0xFFFFFFFF) so the
        // key stays compact + hashable without a tuple key.
        [[nodiscard]] static i64 PackKey(i32 first, i32 second)
        {
            return (static_cast<i64>(first) << 32) | static_cast<i64>(static_cast<u32>(second));
        }

        String m_familyName;
        f32 m_pixelHeight = 0;
        FontMetrics m_metrics;
        HashMap<i32, GlyphInfo> m_glyphs;
        HashMap<i64, f32> m_kerning;
    };

    // IFontAtlas backed by a pre-rasterized 8-bit alpha buffer + region table.
    class BakedFontAtlas final : public IFontAtlas
    {
    public:
        BakedFontAtlas() = default;

        /// The pack-time oversampling factors (stb packs glyph BITMAPS at oversample times the
        /// logical size for better filtering; packedchar offsets/advances stay LOGICAL). Screen
        /// quads must divide the raw region span back down - losing this factor was the
        /// 2026-08-12 jumbled-text bug (glyphs drawn at 2x, overlapping). 1 = no oversampling.
        void SetOversample(f32 x, f32 y)
        {
            m_oversampleX = x > 0.0f ? x : 1.0f;
            m_oversampleY = y > 0.0f ? y : 1.0f;
        }
        [[nodiscard]] f32 OversampleX() const { return m_oversampleX; }
        [[nodiscard]] f32 OversampleY() const { return m_oversampleY; }

        // Clears all glyph regions + pixel buffer in place for hot-reload.
        void ClearForReload()
        {
            m_regions.Clear();
            m_pixels.Clear();
            m_width = 0;
            m_height = 0;
            m_whitePixelU = 0;
            m_whitePixelV = 0;
        }

        // Take ownership of a `width * height`-byte single-channel buffer,
        // replacing any buffer currently held.
        void SetPixels(u32 width, u32 height, Array<u8>&& takenPixels)
        {
            m_width = width;
            m_height = height;
            m_pixels = Move(takenPixels);
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

        // --- IFontAtlas ----------------------------------------------------
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

        // Keyed by codepoint for now: a codepoint is its own glyph id.
        [[nodiscard]] i32 GlyphFor(i32 codepoint) const override { return codepoint; }
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

            // Screen-space quad: position + offsets + dimensions. Region width/height are RAW
            // atlas pixels (oversampled); offsets/advance are logical - divide the span only
            // (exactly stbtt_GetPackedQuad's correction).
            const f32 qx0 = x + region.offsetX;
            const f32 qy0 = y + region.offsetY;
            const f32 qx1 = qx0 + static_cast<f32>(region.width) / m_oversampleX;
            const f32 qy1 = qy0 + static_cast<f32>(region.height) / m_oversampleY;

            // Texture-space quad: region bounds normalized into the atlas.
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
        f32 m_oversampleX = 1.0f;
        f32 m_oversampleY = 1.0f;
        Array<u8> m_pixels;
        HashMap<i32, AtlasRegion> m_regions;
        f32 m_whitePixelU = 0;
        f32 m_whitePixelV = 0;
    };
}
