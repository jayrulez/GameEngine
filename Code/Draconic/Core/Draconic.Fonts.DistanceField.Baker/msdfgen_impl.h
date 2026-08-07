// msdfgen wrapper - plain C++ header (no modules). Called from the baker module
// partition through the global module fragment.
#pragma once

#include <cstdint>

namespace draconic::fonts::df
{

    using u8 = uint8_t;
    using i32 = int32_t;
    using f64 = double;

    // Generate an MSDF bitmap for a single glyph and write RGBA8 pixels to rgbaOut.
    // Returns false on failure (e.g. glyph not found).
    bool GenerateGlyphMSDF(const u8* fontData, i32 fontDataSize, i32 codepoint, i32 width,
                           i32 height, f64 pxRange, f64 scaleX, f64 scaleY, f64 translateX,
                           f64 translateY, u8* rgbaOut);

} // namespace draconic::fonts::df
