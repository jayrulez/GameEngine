// msdfgen wrapper — C++ TU that includes msdfgen headers.
// Builds msdfgen::Shape from stb_truetype glyph vertices and generates MSDF bitmaps.

#include "msdfgen_impl.h"

#include <msdfgen.h>

#include <stb_truetype.h>

namespace raptor::fonts::df
{

// Build an msdfgen::Shape from stb_truetype glyph vertices.
// Coordinates are in raw font units (Y-up).
static bool BuildShapeFromStb(const stbtt_fontinfo* info, i32 glyphIndex, msdfgen::Shape& shape)
{
    stbtt_vertex* vertices = nullptr;
    const int numVerts = stbtt_GetGlyphShape(info, glyphIndex, &vertices);
    if (numVerts <= 0 || !vertices)
        return false;

    msdfgen::Contour* contour = nullptr;
    msdfgen::Point2 cursor(0, 0);

    for (int i = 0; i < numVerts; ++i) {
        const stbtt_vertex& v = vertices[i];
        // Negate Y to convert from TrueType Y-up to Y-down.
        const msdfgen::Point2 p(static_cast<double>(v.x), static_cast<double>(-v.y));

        switch (v.type) {
        case STBTT_vmove:
            contour = &shape.addContour();
            cursor = p;
            break;
        case STBTT_vline:
            if (contour)
                contour->addEdge(msdfgen::EdgeHolder(cursor, p));
            cursor = p;
            break;
        case STBTT_vcurve: {
            const msdfgen::Point2 ctrl(static_cast<double>(v.cx), static_cast<double>(-v.cy));
            if (contour)
                contour->addEdge(msdfgen::EdgeHolder(cursor, ctrl, p));
            cursor = p;
            break;
        }
        case STBTT_vcubic: {
            const msdfgen::Point2 ctrl1(static_cast<double>(v.cx), static_cast<double>(-v.cy));
            const msdfgen::Point2 ctrl2(static_cast<double>(v.cx1), static_cast<double>(-v.cy1));
            if (contour)
                contour->addEdge(msdfgen::EdgeHolder(cursor, ctrl1, ctrl2, p));
            cursor = p;
            break;
        }
        }
    }

    stbtt_FreeShape(info, vertices);
    return shape.contours.size() > 0;
}

bool GenerateGlyphMSDF(const u8* fontData, i32 fontDataSize,
                       i32 codepoint, i32 width, i32 height, f64 pxRange,
                       f64 scaleX, f64 scaleY, f64 translateX, f64 translateY,
                       u8* rgbaOut)
{
    (void)fontDataSize;

    stbtt_fontinfo info;
    if (!stbtt_InitFont(&info, fontData, stbtt_GetFontOffsetForIndex(fontData, 0)))
        return false;

    const int glyphIndex = stbtt_FindGlyphIndex(&info, codepoint);
    if (glyphIndex <= 0)
        return false;

    msdfgen::Shape shape;
    if (!BuildShapeFromStb(&info, glyphIndex, shape))
        return false;

    shape.inverseYAxis = false;  // We already negated Y in shape vertices.
    shape.normalize();
    msdfgen::edgeColoringByDistance(shape, 3.0);

    msdfgen::Bitmap<float, 3> msdf(width, height);

    // SDFTransformation: Projection maps shape coords (font units) to pixel coords.
    // DistanceMapping (Range) converts shape-unit distances to [0,1].
    // Range must be in FONT UNITS: pxRange pixels = pxRange/scale font units.
    const double shapeRange = pxRange / scaleX;
    msdfgen::SDFTransformation transform(
        msdfgen::Projection(msdfgen::Vector2(scaleX, scaleY),
                            msdfgen::Vector2(translateX, translateY)),
        msdfgen::Range(shapeRange));
    msdfgen::generateMSDF(msdf, shape, transform);

    // Convert float RGB -> RGBA8 (A = 255).
    // Shape Y was negated (Y-down). In msdfgen's bitmap, low Y = low row index.
    // Since shape Y is already Y-down, low shape Y (glyph top) maps to low rows
    // (top of output image). No flip needed.
    for (i32 y = 0; y < height; ++y) {
        for (i32 x = 0; x < width; ++x) {
            const float* pixel = msdf(x, y);
            const i32 idx = (y * width + x) * 4;
            auto clampByte = [](float v) -> u8 {
                return static_cast<u8>(v < 0.0f ? 0.0f : (v > 255.0f ? 255.0f : v));
            };
            rgbaOut[idx + 0] = clampByte(pixel[0] * 255.0f);
            rgbaOut[idx + 1] = clampByte(pixel[1] * 255.0f);
            rgbaOut[idx + 2] = clampByte(pixel[2] * 255.0f);
            rgbaOut[idx + 3] = 255;
        }
    }

    return true;
}

} // namespace raptor::fonts::df
