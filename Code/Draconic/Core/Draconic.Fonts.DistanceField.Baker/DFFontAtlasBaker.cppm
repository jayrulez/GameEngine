// Draconic::FontsDFBaker - draconic.fonts.distancefield.baker:baker partition
//
// IFontAtlasBaker that generates MSDF atlases via msdfgen (core-only).
// Extracts glyph outlines from TrueTypeFont raw data via stb_truetype,
// generates per-glyph MSDF bitmaps, and packs them into a DFFontAtlas.

module;
#include "Draconic.Foundation/Prelude.h"
#include "msdfgen_impl.h"

#define STBTT_DEF extern
#include <stb_truetype.h>

#include <cmath>
#include <cstring>

export module draconic.fonts.distancefield.baker:baker;

import draconic.foundation;
import draconic.fonts;
import draconic.fonts.io;
import draconic.fonts.distancefield;
import draconic.fonts.ttf;

using namespace draconic::foundation;

namespace
{
    constexpr f64 kDefaultPxRange = 4.0;

    // Cleared texels left between packed cells. The atlas starts zeroed (fully "outside"), so a
    // gutter guarantees bilinear sampling at a glyph's UV edge blends into empty space, never into
    // the neighbouring glyph - which otherwise shows as flickering seams under magnification.
    constexpr u32 kCellGutter = 2;

    // Simple row-based atlas packer (leaves a gutter between cells).
    struct RowPacker
    {
        u32 atlasW = 0, atlasH = 0;
        u32 cursorX = 0, cursorY = 0, rowHeight = 0;

        bool TryPack(u32 w, u32 h, u32& outX, u32& outY)
        {
            if (cursorX + w > atlasW)
            {
                cursorX = 0;
                cursorY += rowHeight + kCellGutter;
                rowHeight = 0;
            }
            if (cursorY + h > atlasH)
                return false;
            outX = cursorX;
            outY = cursorY;
            cursorX += w + kCellGutter;
            if (h > rowHeight)
                rowHeight = h;
            return true;
        }
    };
}

export namespace draconic::fonts
{

    class DFFontAtlasBaker final : public IFontAtlasBaker
    {
    public:
        [[nodiscard]] Span<const StringView> SupportedExtensions() const override
        {
            static const StringView exts[] = {u8".ttf", u8".otf", u8".ttc"};
            return Span<const StringView>(exts, 3);
        }

        [[nodiscard]] bool SupportsExtension(StringView ext) const override
        {
            for (const StringView e : SupportedExtensions())
                if (e == ext)
                    return true;
            return false;
        }

        [[nodiscard]] bool CanBake(const IFont& /*font*/) const override
        {
            return false; // require two-arg form with atlas mode check
        }

        [[nodiscard]] bool CanBake(const IFont& font, const FontLoadOptions& opts) const override
        {
            return font.BackendTypeId() == draconic::fonts::kTrueTypeFontTypeId &&
                   opts.atlasMode == AtlasMode::DistanceField;
        }

        [[nodiscard]] Result<IFontAtlas*, FontLoadResult> Bake(IFont& font,
                                                               FontLoadOptions options) override
        {
            if (!CanBake(font, options))
                return Err(FontLoadResult::UnsupportedFormat);

            const auto& ttf = static_cast<const TrueTypeFont&>(font);
            const unsigned char* rawData = ttf.RawData();
            const auto rawDataSize = static_cast<df::i32>(ttf.RawDataSize());

            const u32 atlasW = options.atlasWidth;
            const u32 atlasH = options.atlasHeight;
            const u32 padding = options.padding;
            const f64 pxRange = kDefaultPxRange;
            const f32 pixelHeight = options.pixelHeight;

            // Init stb_truetype for metrics.
            stbtt_fontinfo stbFont;
            if (!stbtt_InitFont(&stbFont, rawData, stbtt_GetFontOffsetForIndex(rawData, 0)))
                return Err(FontLoadResult::InvalidFormat);

            const f32 scale = stbtt_ScaleForPixelHeight(&stbFont, pixelHeight);

            // Allocate atlas pixel buffer (RGBA8).
            Array<u8> pixels(static_cast<usize>(atlasW) * atlasH * 4);
            MemSet(pixels.Data(), 0, pixels.Size());

            DFFontAtlas* atlas = DefaultAllocator().New<DFFontAtlas>();
            atlas->SetPixelRange(static_cast<f32>(pxRange));

            RowPacker packer;
            packer.atlasW = atlasW;
            packer.atlasH = atlasH;

            bool anyGlyphs = false;

            for (i32 cp = options.firstCodepoint; cp <= options.lastCodepoint; ++cp)
            {
                const int glyphIdx = stbtt_FindGlyphIndex(&stbFont, cp);
                if (glyphIdx <= 0)
                    continue;

                // Blank glyphs (space, NBSP - no outline) still carry an advance. Record an
                // advance-only region or the cursor-walk draw paths, which step by
                // region.advanceX, render "hello world" as "helloworld". The raster baker
                // gets this for free from stb's packed chars.
                const auto addAdvanceOnlyRegion = [&](i32 codepoint, int glyphIndex)
                {
                    int advW, lsb;
                    stbtt_GetGlyphHMetrics(&stbFont, glyphIndex, &advW, &lsb);
                    atlas->SetRegion(codepoint, AtlasRegion(0, 0, 0, 0, 0.0f, 0.0f,
                                                            static_cast<f32>(advW) * scale));
                };

                // Get pixel-space bitmap box from stb (Y-down screen coords, consistent
                // with stb's scale). Also get font-unit bbox for the msdfgen projection.
                int ix0, iy0, ix1, iy1;
                stbtt_GetGlyphBitmapBox(&stbFont, glyphIdx, scale, scale, &ix0, &iy0, &ix1, &iy1);

                int fuX0, fuY0, fuX1, fuY1;
                if (!stbtt_GetGlyphBox(&stbFont, glyphIdx, &fuX0, &fuY0, &fuX1, &fuY1))
                {
                    addAdvanceOnlyRegion(cp, glyphIdx);
                    continue;
                }

                const f64 s = static_cast<f64>(scale);
                const i32 pad = static_cast<i32>(padding);

                const i32 glyphW = ix1 - ix0;
                const i32 glyphH = iy1 - iy0;
                if (glyphW <= 0 || glyphH <= 0)
                {
                    addAdvanceOnlyRegion(cp, glyphIdx);
                    continue;
                }

                // Cell = glyph + padding on each side + 1px safety margin.
                const i32 cellW = glyphW + pad * 2 + 2;
                const i32 cellH = glyphH + pad * 2 + 2;

                u32 packX = 0, packY = 0;
                if (!packer.TryPack(static_cast<u32>(cellW), static_cast<u32>(cellH), packX, packY))
                    continue;

                // msdfgen's Projection is scale*(coord + translate), so translate is in FONT
                // UNITS (added before scaling). Map the glyph bbox min corner (fuX0, fuY0) to
                // pixel (pad+1, pad+1): translate = (pad+1)/s - bboxMin. The msdfgen bitmap is
                // Y-up (row 0 = bottom); the output loop flips rows to the top-down atlas
                // convention, which lands the descender (low font Y) near the cell's bottom.
                const f64 translateX = static_cast<f64>(pad + 1) / s - static_cast<f64>(fuX0);
                const f64 translateY = static_cast<f64>(pad + 1) / s - static_cast<f64>(fuY0);

                // Generate the MSDF into a temp buffer.
                Array<u8> cellPixels(static_cast<usize>(cellW) * cellH * 4);
                MemSet(cellPixels.Data(), 0, cellPixels.Size());

                const bool ok =
                    df::GenerateGlyphMSDF(rawData, rawDataSize, cp, cellW, cellH, pxRange, s, s,
                                          translateX, translateY, cellPixels.Data());

                if (!ok)
                    continue;

                // Blit cell into atlas.
                for (i32 row = 0; row < cellH; ++row)
                {
                    const usize srcOff = static_cast<usize>(row) * cellW * 4;
                    const usize dstOff = (static_cast<usize>(packY + row) * atlasW + packX) * 4;
                    MemCopy(pixels.Data() + dstOff, cellPixels.Data() + srcOff,
                            static_cast<usize>(cellW) * 4);
                }

                // Advance width.
                int advW, lsb;
                stbtt_GetGlyphHMetrics(&stbFont, glyphIdx, &advW, &lsb);

                // AtlasRegion offsets: cell top-left relative to cursor baseline
                // in Y-down screen coords. ix0/iy0 are the glyph's pixel-space
                // top-left offset from baseline; subtract (pad+1) for the cell margin.
                const f32 offsetX = static_cast<f32>(ix0 - pad - 1);
                const f32 offsetY = static_cast<f32>(iy0 - pad - 1);

                AtlasRegion region(static_cast<u16>(packX), static_cast<u16>(packY),
                                   static_cast<u16>(cellW), static_cast<u16>(cellH), offsetX,
                                   offsetY, static_cast<f32>(advW) * scale);
                atlas->SetRegion(cp, region);
                anyGlyphs = true;
            }

            if (!anyGlyphs)
            {
                DefaultAllocator().Delete(atlas);
                return Err(FontLoadResult::NoGlyphsFound);
            }

            // Write a 2x2 solid white block at bottom-right for solid-color draws.
            {
                const u32 wx = atlasW - 2;
                const u32 wy = atlasH - 2;
                for (u32 dy = 0; dy < 2; ++dy)
                {
                    for (u32 dx = 0; dx < 2; ++dx)
                    {
                        const usize idx = (static_cast<usize>(wy + dy) * atlasW + wx + dx) * 4;
                        pixels[idx + 0] = 255;
                        pixels[idx + 1] = 255;
                        pixels[idx + 2] = 255;
                        pixels[idx + 3] = 255;
                    }
                }
                const f32 whiteU = (static_cast<f32>(wx) + 0.5f) / static_cast<f32>(atlasW);
                const f32 whiteV = (static_cast<f32>(wy) + 0.5f) / static_cast<f32>(atlasH);
                atlas->SetWhitePixelUV(whiteU, whiteV);
            }

            atlas->SetPixels(atlasW, atlasH, Move(pixels));
            return static_cast<IFontAtlas*>(atlas);
        }
    };

} // namespace draconic::fonts
