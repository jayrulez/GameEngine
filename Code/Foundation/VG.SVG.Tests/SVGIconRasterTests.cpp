// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// An icon at the size it is drawn, rasterized here as the GPU samples the batch (each pixel's
// centre, coverage interpolated across a triangle, drawn over in order, a centre on an edge two
// triangles share drawn by one of them): a symmetric icon comes out
// symmetric, and its edges are anti-aliased (partial coverage), not stepped.

#include <doctest/doctest.h>
#include <string>
#include "Core/Prelude.h"

import foundation.core;
import foundation.vg;
import foundation.vg.svg;

using namespace foundation::core;
namespace vg = foundation::vg;
namespace svg = foundation::vg::svg;

namespace
{
    // The batch's triangles into a size x size alpha grid, sampled at pixel centres.
    Array<f32> Rasterize(const vg::VGBatch& batch, i32 size)
    {
        Array<f32> alpha;
        alpha.Resize(static_cast<usize>(size * size), 0.0f);
        for (usize t = 0; t + 2 < batch.indices.Size(); t += 3)
        {
            const vg::VGVertex& a = batch.vertices[batch.indices[t]];
            const vg::VGVertex& b = batch.vertices[batch.indices[t + 1]];
            const vg::VGVertex& c = batch.vertices[batch.indices[t + 2]];
            const f32 area = (b.position.x - a.position.x) * (c.position.y - a.position.y) - (c.position.x - a.position.x) * (b.position.y - a.position.y);
            if (Abs(area) < 1e-9f)
                continue;
            for (i32 y = 0; y < size; ++y)
                for (i32 x = 0; x < size; ++x)
                {
                    // Nudged off the centre, as the GPU's fill rule breaks a tie: a centre exactly on
                    // a shared edge would otherwise be drawn by both triangles.
                    const Float2 p{static_cast<f32>(x) + 0.5f + 1e-4f, static_cast<f32>(y) + 0.5f + 3e-4f};
                    const f32 w0 = ((b.position.x - p.x) * (c.position.y - p.y) - (c.position.x - p.x) * (b.position.y - p.y)) / area;
                    const f32 w1 = ((c.position.x - p.x) * (a.position.y - p.y) - (a.position.x - p.x) * (c.position.y - p.y)) / area;
                    const f32 w2 = 1.0f - w0 - w1;
                    if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f)
                        continue;
                    const f32 coverage = w0 * a.coverage + w1 * b.coverage + w2 * c.coverage;
                    const f32 src = coverage * (w0 * a.color.a + w1 * b.color.a + w2 * c.color.a);
                    f32& dst = alpha[static_cast<usize>(y * size + x)];
                    dst = src + dst * (1.0f - src); // over
                }
        }
        return alpha;
    }

    std::string Picture(const Array<f32>& alpha, i32 size)
    {
        std::string out;
        for (i32 y = 0; y < size; ++y)
        {
            for (i32 x = 0; x < size; ++x)
            {
                char cell[6];
                std::snprintf(cell, sizeof(cell), "%4d", static_cast<int>(alpha[static_cast<usize>(y * size + x)] * 100.0f + 0.5f));
                out += cell;
            }
            out += '\n';
        }
        return out;
    }
}

TEST_CASE("svg icon raster: symmetric stroked icons are symmetric and anti-aliased at icon size")
{
    // The editor's close and plus icons: round-capped strokes, corner to corner and across.
    const StringView icons[] = {
        u8"<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 16 16\" fill=\"none\" stroke=\"#ffffff\" stroke-width=\"1.5\" "
        u8"stroke-linecap=\"round\" stroke-linejoin=\"round\"><path d=\"M4 4l8 8M12 4l-8 8\"/></svg>",
        u8"<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 16 16\" fill=\"none\" stroke=\"#ffffff\" stroke-width=\"1.5\" "
        u8"stroke-linecap=\"round\" stroke-linejoin=\"round\"><path d=\"M8 3v10M3 8h10\"/></svg>",
    };
    for (const StringView icon : icons)
    {
        Result<svg::SVGDocument> parsed = svg::SVGLoader::Load(icon);
        REQUIRE(parsed.HasValue());
        for (const i32 size : {16, 14})
        {
            vg::VGContext context;
            svg::SVGRenderer::Render(context, parsed.Value(), Rectangle{0.0f, 0.0f, static_cast<f32>(size), static_cast<f32>(size)});
            const Array<f32> alpha = Rasterize(context.GetBatch(), size);
            MESSAGE("size " << size << "\n" << Picture(alpha, size));
            f32 worstMirror = 0.0f;
            usize partial = 0;
            for (i32 y = 0; y < size; ++y)
                for (i32 x = 0; x < size; ++x)
                {
                    const f32 here = alpha[static_cast<usize>(y * size + x)];
                    worstMirror = Max(worstMirror, Abs(here - alpha[static_cast<usize>(y * size + (size - 1 - x))])); // left to right
                    worstMirror = Max(worstMirror, Abs(here - alpha[static_cast<usize>((size - 1 - y) * size + x)])); // top to bottom
                    partial += here > 0.1f && here < 0.9f ? 1u : 0u;
                }
            CHECK(worstMirror < 0.15f);
            CHECK(partial >= 8u); // its edges graded
        }
    }
}
