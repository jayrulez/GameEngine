// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Single translation unit for the stb_truetype implementation.
// Module units cannot define this macro (static symbol collisions), so the
// implementation lives in a regular .cpp linked into Foundation::Fonts.TrueType.
// The glyph packer's declaration first: stb_truetype.h compiles its implementation on the include
// that sees the define, which must be the only one.
#include "stb_truetype_glyphs.h"
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"


int EngineStbPackGlyphs(stbtt_pack_context* spc, const stbtt_fontinfo* info, float pixelHeight, const int* glyphs,
                        int count, stbtt_packedchar* out)
{
    if (count <= 0)
    {
        return 1;
    }
    const float scale = stbtt_ScaleForPixelHeight(info, pixelHeight);
    const int hOver = static_cast<int>(spc->h_oversample);
    const int vOver = static_cast<int>(spc->v_oversample);
    stbrp_rect* rects =
        static_cast<stbrp_rect*>(STBTT_malloc(sizeof(stbrp_rect) * static_cast<size_t>(count), spc->user_allocator_context));
    if (rects == nullptr)
    {
        return 0;
    }
    // Gather (stbtt_PackFontRangesGatherRects, by index).
    for (int k = 0; k < count; ++k)
    {
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        stbtt_GetGlyphBitmapBoxSubpixel(info, glyphs[k], scale * static_cast<float>(hOver),
                                        scale * static_cast<float>(vOver), 0, 0, &x0, &y0, &x1, &y1);
        rects[k].id = k;
        rects[k].was_packed = 0;
        if (x1 <= x0 || y1 <= y0)
        {
            rects[k].w = rects[k].h = 0; // blank (a space): nothing to pack, only an advance
            continue;
        }
        rects[k].w = static_cast<stbrp_coord>(x1 - x0 + spc->padding + hOver - 1);
        rects[k].h = static_cast<stbrp_coord>(y1 - y0 + spc->padding + vOver - 1);
    }
    stbtt_PackFontRangesPackRects(spc, rects, count);

    // Render (stbtt_PackFontRangesRenderIntoRects, by index).
    int result = 1;
    const float recipH = 1.0f / static_cast<float>(hOver);
    const float recipV = 1.0f / static_cast<float>(vOver);
    const float subX = stbtt__oversample_shift(hOver);
    const float subY = stbtt__oversample_shift(vOver);
    for (int k = 0; k < count; ++k)
    {
        stbrp_rect* r = &rects[k];
        stbtt_packedchar* bc = &out[k];
        const int glyph = glyphs[k];
        int advance = 0, lsb = 0, x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        if (r->w == 0 || r->h == 0)
        {
            // Blank: a zero-area record that still advances.
            stbtt_GetGlyphHMetrics(info, glyph, &advance, &lsb);
            *bc = stbtt_packedchar{};
            bc->xadvance = scale * static_cast<float>(advance);
            continue;
        }
        if (!r->was_packed)
        {
            *bc = stbtt_packedchar{};
            result = 0;
            continue;
        }
        const stbrp_coord pad = static_cast<stbrp_coord>(spc->padding);
        r->x += pad;
        r->y += pad;
        r->w -= pad;
        r->h -= pad;
        stbtt_GetGlyphHMetrics(info, glyph, &advance, &lsb);
        stbtt_GetGlyphBitmapBox(info, glyph, scale * static_cast<float>(hOver), scale * static_cast<float>(vOver), &x0,
                                &y0, &x1, &y1);
        stbtt_MakeGlyphBitmapSubpixel(info, spc->pixels + r->x + r->y * spc->stride_in_bytes, r->w - hOver + 1,
                                      r->h - vOver + 1, spc->stride_in_bytes, scale * static_cast<float>(hOver),
                                      scale * static_cast<float>(vOver), 0, 0, glyph);
        if (hOver > 1)
        {
            stbtt__h_prefilter(spc->pixels + r->x + r->y * spc->stride_in_bytes, r->w, r->h, spc->stride_in_bytes,
                               static_cast<unsigned int>(hOver));
        }
        if (vOver > 1)
        {
            stbtt__v_prefilter(spc->pixels + r->x + r->y * spc->stride_in_bytes, r->w, r->h, spc->stride_in_bytes,
                               static_cast<unsigned int>(vOver));
        }
        bc->x0 = static_cast<stbtt_int16>(r->x);
        bc->y0 = static_cast<stbtt_int16>(r->y);
        bc->x1 = static_cast<stbtt_int16>(r->x + r->w);
        bc->y1 = static_cast<stbtt_int16>(r->y + r->h);
        bc->xadvance = scale * static_cast<float>(advance);
        bc->xoff = static_cast<float>(x0) * recipH + subX;
        bc->yoff = static_cast<float>(y0) * recipV + subY;
        bc->xoff2 = static_cast<float>(x0 + r->w) * recipH + subX;
        bc->yoff2 = static_cast<float>(y0 + r->h) * recipV + subY;
    }
    STBTT_free(rects, spc->user_allocator_context);
    return result;
}
