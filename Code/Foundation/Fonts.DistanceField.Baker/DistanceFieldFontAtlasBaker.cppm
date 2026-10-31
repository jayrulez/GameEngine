// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Fonts.DistanceField.Baker - foundation.fonts.distancefield.baker:baker partition
//
// IFontAtlasBaker that generates MTSDF atlases via msdfgen (core-only).
// Extracts glyph outlines from TrueTypeFont raw data via stb_truetype,
// generates per-glyph MTSDF bitmaps (the multi-channel field in RGB, the true
// distance in alpha for text effects), and packs them into a DistanceFieldFontAtlas.

module;
#include "Core/Prelude.h"
#include "msdfgen_impl.h"

#define STBTT_DEF extern
#include <stb_truetype.h>

#include <cmath>
#include <cstring>

export module foundation.fonts.distancefield.baker:baker;

import foundation.core;
import foundation.fonts;
import foundation.fonts.distancefield;
import foundation.fonts.truetype;

using namespace foundation::core;

namespace
{
    constexpr f64 kDefaultPxRange = 4.0;

    // Cleared texels left between packed cells. The atlas starts zeroed (fully "outside"), so a
    // gutter guarantees bilinear sampling at a glyph's UV edge blends into empty space, never into
    // the neighbouring glyph, which would show as flickering seams under magnification.
    constexpr u32 kCellGutter = 2;

    // The solid block for solid-colour draws, packed as a cell like any glyph.
    constexpr u32 kWhiteBlockSize = 2;

    // Atlas sides are rounded up to this, so the atlas stays block-compressible (4x4 blocks).
    constexpr u32 kAtlasSideMultiple = 4;

    // The narrowest width tried when sizing the atlas to its glyphs.
    constexpr u32 kSmallestTriedWidth = 64;

    [[nodiscard]] constexpr u32 RoundUp(u32 value, u32 multiple)
    {
        return (value + multiple - 1) / multiple * multiple;
    }

    // Simple row-based atlas packer (leaves a gutter between cells).
    struct RowPacker
    {
        u32 atlasW = 0, atlasH = 0;
        u32 cursorX = 0, cursorY = 0, rowHeight = 0;
        u32 usedW = 0, usedH = 0; // the extent the packed cells reach

        bool TryPack(u32 w, u32 h, u32& outX, u32& outY)
        {
            if (cursorX + w > atlasW)
            {
                cursorX = 0;
                cursorY += rowHeight + kCellGutter;
                rowHeight = 0;
            }
            if (w > atlasW || cursorY + h > atlasH)
                return false;
            outX = cursorX;
            outY = cursorY;
            cursorX += w + kCellGutter;
            if (h > rowHeight)
                rowHeight = h;
            if (outX + w > usedW)
                usedW = outX + w;
            if (outY + h > usedH)
                usedH = outY + h;
            return true;
        }
    };

    struct CellSize
    {
        u32 w = 0, h = 0;
    };

    struct CellPos
    {
        u32 x = 0, y = 0;
    };

    // Packs every cell, in order, into an atlas `width` wide and at most `maxH` tall. Fills `outPos`
    // (one per cell) and the atlas size the cells need, rounded up to kAtlasSideMultiple; false if
    // they do not all fit.
    [[nodiscard]] bool PackAll(const Array<CellSize>& cells, u32 width, u32 maxH, Array<CellPos>* outPos,
                               u32& outW, u32& outH)
    {
        RowPacker packer;
        packer.atlasW = width;
        packer.atlasH = maxH;
        for (usize i = 0; i < cells.Size(); ++i)
        {
            u32 x = 0, y = 0;
            if (!packer.TryPack(cells[i].w, cells[i].h, x, y))
                return false;
            if (outPos != nullptr)
                (*outPos)[i] = CellPos{x, y};
        }
        outW = RoundUp(packer.usedW, kAtlasSideMultiple);
        outH = RoundUp(packer.usedH, kAtlasSideMultiple);
        return outW <= width && outH <= maxH;
    }

    // The width (at most maxW) whose packing of `cells` needs the smallest atlas, preferring the
    // squarer atlas on a tie. The asset's atlas size is a maximum: a Latin set at 48 px needs a
    // fraction of 1024 x 1024, and every texel of it ships. False if the cells do not fit at all.
    [[nodiscard]] bool ChooseAtlasWidth(const Array<CellSize>& cells, u32 maxW, u32 maxH, u32& outWidth)
    {
        u64 bestArea = 0;
        u32 bestSkew = 0;
        bool found = false;
        const auto consider = [&](u32 width)
        {
            u32 w = 0, h = 0;
            if (!PackAll(cells, width, maxH, nullptr, w, h))
                return;
            const u64 area = static_cast<u64>(w) * h;
            const u32 skew = w > h ? w - h : h - w;
            if (!found || area < bestArea || (area == bestArea && skew < bestSkew))
            {
                found = true;
                bestArea = area;
                bestSkew = skew;
                outWidth = width;
            }
        };
        for (u32 width = kSmallestTriedWidth; width < maxW; width *= 2)
            consider(width);
        consider(maxW);
        return found;
    }
}

export namespace foundation::fonts
{

    class DistanceFieldFontAtlasBaker final : public IFontAtlasBaker
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
            return font.BackendTypeId() == foundation::fonts::kTrueTypeFontTypeId &&
                   opts.atlasMode == AtlasMode::DistanceField;
        }

        [[nodiscard]] Result<IFontAtlas*, FontLoadResult>
        Bake(IFont& font, FontLoadOptions options, IAllocator& allocator) override
        {
            if (!CanBake(font, options))
                return Err(FontLoadResult::UnsupportedFormat);

            const auto& ttf = static_cast<const TrueTypeFont&>(font);
            const unsigned char* rawData = ttf.RawData();
            const auto rawDataSize = static_cast<df::i32>(ttf.RawDataSize());

            // The asset's atlas size is the most the atlas may take; it is cropped to its glyphs.
            const u32 maxAtlasW = options.atlasWidth;
            const u32 maxAtlasH = options.atlasHeight;
            const f64 pxRange = options.distanceFieldRange > 0.0f ? static_cast<f64>(options.distanceFieldRange)
                                                                  : kDefaultPxRange;
            // The cell holds the field's reach past the glyph's box (half the range), so an effect
            // drawn out to it is not cut at the cell's edge.
            const u32 padding = Max(static_cast<u32>(options.padding), static_cast<u32>(Ceil(static_cast<f32>(pxRange) * 0.5f)));
            const f32 pixelHeight = options.pixelHeight;

            // Init stb_truetype for metrics.
            stbtt_fontinfo stbFont;
            if (!stbtt_InitFont(&stbFont, rawData, stbtt_GetFontOffsetForIndex(rawData, 0)))
                return Err(FontLoadResult::InvalidFormat);

            const f32 scale = stbtt_ScaleForPixelHeight(&stbFont, pixelHeight);

            DistanceFieldFontAtlas* atlas = allocator.New<DistanceFieldFontAtlas>();
            atlas->SetPixelRange(static_cast<f32>(pxRange));

            // Phase 1 (sequential): metrics, then deterministic packing into an atlas sized to
            // the glyphs. The MSDF generation itself is the expensive part and each glyph is
            // independent, so it runs in phase 2 as a ParallelFor over the packed work list;
            // the pack order (and so the atlas layout) never depends on worker scheduling.
            struct GlyphWork
            {
                i32 glyph = 0; // the font's glyph index
                i32 cellW = 0, cellH = 0;
                u32 packX = 0, packY = 0;
                f64 translateX = 0.0, translateY = 0.0;
                AtlasRegion region;
                bool generated = false;
            };
            Array<GlyphWork> work;

            // The glyphs the range maps to, each once, keyed by glyph index; the range's codepoints
            // map to them for unshaped text.
            Array<i32> glyphs;
            for (i32 cp = options.firstCodepoint; cp <= options.lastCodepoint; ++cp)
            {
                const int glyphIdx = stbtt_FindGlyphIndex(&stbFont, cp);
                if (glyphIdx <= 0)
                    continue;
                atlas->SetGlyphFor(cp, glyphIdx);
                bool seen = false;
                for (const i32 g : glyphs)
                    seen = seen || g == glyphIdx;
                if (!seen)
                    glyphs.PushBack(glyphIdx);
            }
            // Then what the registered shaper's substitutions reach from them.
            for (const i32 reachable : ReachableGlyphs(font, options, allocator))
            {
                bool seen = false;
                for (const i32 g : glyphs)
                    seen = seen || g == reachable;
                if (!seen && reachable > 0 && reachable < stbFont.numGlyphs)
                    glyphs.PushBack(reachable);
            }
            for (const i32 glyphIdx : glyphs)
            {

                // Blank glyphs (space, NBSP: no outline) still carry an advance. Record an
                // advance-only region or the cursor-walk draw paths, which step by
                // region.advanceX, render "hello world" as "helloworld". The raster baker
                // gets this for free from stb's packed chars.
                const auto addAdvanceOnlyRegion = [&](int glyphIndex)
                {
                    int advW, lsb;
                    stbtt_GetGlyphHMetrics(&stbFont, glyphIndex, &advW, &lsb);
                    atlas->SetRegion(glyphIndex, AtlasRegion(0, 0, 0, 0, 0.0f, 0.0f,
                                                             static_cast<f32>(advW) * scale));
                };

                // Get pixel-space bitmap box from stb (Y-down screen coords, consistent
                // with stb's scale). Also get font-unit bbox for the msdfgen projection.
                int ix0, iy0, ix1, iy1;
                stbtt_GetGlyphBitmapBox(&stbFont, glyphIdx, scale, scale, &ix0, &iy0, &ix1, &iy1);

                int fuX0, fuY0, fuX1, fuY1;
                if (!stbtt_GetGlyphBox(&stbFont, glyphIdx, &fuX0, &fuY0, &fuX1, &fuY1))
                {
                    addAdvanceOnlyRegion(glyphIdx);
                    continue;
                }

                const f64 s = static_cast<f64>(scale);
                const i32 pad = static_cast<i32>(padding);

                const i32 glyphW = ix1 - ix0;
                const i32 glyphH = iy1 - iy0;
                if (glyphW <= 0 || glyphH <= 0)
                {
                    addAdvanceOnlyRegion(glyphIdx);
                    continue;
                }

                // Cell = glyph, plus padding on each side, plus a 1px safety margin.
                const i32 cellW = glyphW + pad * 2 + 2;
                const i32 cellH = glyphH + pad * 2 + 2;

                // Advance width.
                int advW, lsb;
                stbtt_GetGlyphHMetrics(&stbFont, glyphIdx, &advW, &lsb);

                // AtlasRegion offsets: cell top-left relative to cursor baseline
                // in Y-down screen coords. ix0/iy0 are the glyph's pixel-space
                // top-left offset from baseline; subtract (pad+1) for the cell margin.
                const f32 offsetX = static_cast<f32>(ix0 - pad - 1);
                const f32 offsetY = static_cast<f32>(iy0 - pad - 1);

                GlyphWork item;
                item.glyph = glyphIdx;
                item.cellW = cellW;
                item.cellH = cellH;
                // msdfgen's Projection is scale*(coord + translate), so translate is in font
                // units (added before scaling). Map the glyph bbox min corner (fuX0, fuY0) to
                // pixel (pad+1, pad+1): translate = (pad+1)/s - bboxMin. The msdfgen bitmap is
                // Y-up (row 0 = bottom); the output loop flips rows to the top-down atlas
                // convention, which lands the descender (low font Y) near the cell's bottom.
                item.translateX = static_cast<f64>(pad + 1) / s - static_cast<f64>(fuX0);
                item.translateY = static_cast<f64>(pad + 1) / s - static_cast<f64>(fuY0);
                item.region = AtlasRegion(0, 0, static_cast<u16>(cellW), static_cast<u16>(cellH), offsetX,
                                          offsetY, static_cast<f32>(advW) * scale);
                work.PushBack(item);
            }

            // Pack the glyph cells and, last, the white block, at the width that needs the least
            // atlas. A glyph that cannot fit fails the bake: dropping it would draw that
            // character as nothing, with no word why.
            Array<CellSize> cells;
            for (const GlyphWork& item : work)
                cells.PushBack(CellSize{static_cast<u32>(item.cellW), static_cast<u32>(item.cellH)});
            cells.PushBack(CellSize{kWhiteBlockSize, kWhiteBlockSize});
            u32 packWidth = 0;
            if (!ChooseAtlasWidth(cells, maxAtlasW, maxAtlasH, packWidth))
            {
                allocator.Delete(atlas);
                return Err(FontLoadResult::AtlasPackingFailed);
            }
            Array<CellPos> positions;
            positions.Resize(cells.Size());
            u32 atlasW = 0, atlasH = 0;
            (void)PackAll(cells, packWidth, maxAtlasH, &positions, atlasW, atlasH);
            for (usize i = 0; i < work.Size(); ++i)
            {
                work[i].packX = positions[i].x;
                work[i].packY = positions[i].y;
                work[i].region.x = static_cast<u16>(positions[i].x);
                work[i].region.y = static_cast<u16>(positions[i].y);
            }
            const CellPos whiteAt = positions[cells.Size() - 1];

            // The atlas pixels (RGBA8), zeroed: fully "outside" every glyph.
            Array<u8> pixels(static_cast<usize>(atlasW) * atlasH * 4);
            MemSet(pixels.Data(), 0, pixels.Size());

            // Phase 2 (parallel): generate each glyph's MTSDF and blit it into its own
            // disjoint atlas rect. GenerateGlyphMTSDF is stateless (per-call font parse),
            // and cells never overlap (the packer leaves a gutter), so concurrent blits
            // touch disjoint bytes.
            if (!work.IsEmpty())
            {
                const auto bakeOne = [&](u32 index)
                {
                    GlyphWork& item = work[static_cast<usize>(index)];
                    Array<u8> cellPixels(static_cast<usize>(item.cellW) * item.cellH * 4);
                    MemSet(cellPixels.Data(), 0, cellPixels.Size());
                    if (!df::GenerateGlyphMTSDF(rawData, rawDataSize, item.glyph, item.cellW,
                                                item.cellH, pxRange, static_cast<f64>(scale),
                                                static_cast<f64>(scale), item.translateX,
                                                item.translateY, cellPixels.Data()))
                    {
                        return;
                    }
                    for (i32 row = 0; row < item.cellH; ++row)
                    {
                        const usize srcOff = static_cast<usize>(row) * item.cellW * 4;
                        const usize dstOff =
                            (static_cast<usize>(item.packY + static_cast<u32>(row)) * atlasW +
                             item.packX) *
                            4;
                        MemCopy(pixels.Data() + dstOff, cellPixels.Data() + srcOff,
                                static_cast<usize>(item.cellW) * 4);
                    }
                    item.generated = true;
                };
                JobSystem jobs(allocator); // scoped pool; zero workers degrades to inline
                jobs.ParallelFor(static_cast<u32>(work.Size()), bakeOne, 1);
            }

            // Phase 3 (sequential): record regions for the glyphs that actually generated
            // (a failed cell must not leave a region pointing at blank texels).
            bool anyGlyphs = false;
            for (const GlyphWork& item : work)
            {
                if (!item.generated)
                    continue;
                atlas->SetRegion(item.glyph, item.region);
                anyGlyphs = true;
            }

            if (!anyGlyphs)
            {
                allocator.Delete(atlas);
                return Err(FontLoadResult::NoGlyphsFound);
            }

            // The solid white block for solid-colour draws, in the cell packed for it.
            {
                const u32 wx = whiteAt.x;
                const u32 wy = whiteAt.y;
                for (u32 dy = 0; dy < kWhiteBlockSize; ++dy)
                {
                    for (u32 dx = 0; dx < kWhiteBlockSize; ++dx)
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

} // namespace foundation::fonts
