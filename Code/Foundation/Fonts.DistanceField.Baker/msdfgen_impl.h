// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// msdfgen wrapper - plain C++ header (no modules). Called from the baker module
// partition through the global module fragment.
#pragma once

#include <cstdint>

namespace foundation::fonts::df
{

    using u8 = uint8_t;
    using i32 = int32_t;
    using f64 = double;

    // Generate an MTSDF bitmap for a single glyph and write RGBA8 pixels to rgbaOut: the
    // multi-channel field in RGB (sharp corners for the glyph itself) and the true signed distance
    // in alpha (round off the edge, what outlines, glows and soft shadows read). Returns false on
    // failure (e.g. glyph not found).
    // `glyphIndex` is the font's glyph index (what atlases key by).
    bool GenerateGlyphMTSDF(const u8* fontData, i32 fontDataSize, i32 glyphIndex, i32 width,
                            i32 height, f64 pxRange, f64 scaleX, f64 scaleY, f64 translateX,
                            f64 translateY, u8* rgbaOut);

} // namespace foundation::fonts::df
