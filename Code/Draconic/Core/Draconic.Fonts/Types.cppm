// Draconic::Fonts - :types partition
//
// Value types for font/glyph/text-layout data. Ported from Sedulous.Fonts
// (Rectangle/GlyphInfo/GlyphQuad/GlyphPosition/AtlasRegion/FontMetrics/
// TextDecorationMetrics/SelectionRange/HitTestResult/FontLoadOptions/
// FontLoadResult/FontCacheKey + alignment enums).

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.fonts:types;

import draconic.foundation;

using namespace draconic::foundation;

export namespace draconic::fonts
{
    // Simple rectangle for glyph bounds.
    struct Rectangle
    {
        f32 x = 0, y = 0, width = 0, height = 0;

        constexpr Rectangle() = default;
        constexpr Rectangle(f32 x_, f32 y_, f32 w_, f32 h_) : x(x_), y(y_), width(w_), height(h_) {}

        [[nodiscard]] constexpr f32 Left() const { return x; }
        [[nodiscard]] constexpr f32 Top() const { return y; }
        [[nodiscard]] constexpr f32 Right() const { return x + width; }
        [[nodiscard]] constexpr f32 Bottom() const { return y + height; }
        [[nodiscard]] constexpr bool IsEmpty() const { return width <= 0 || height <= 0; }
        [[nodiscard]] constexpr bool Contains(f32 px, f32 py) const
        {
            return px >= x && px < x + width && py >= y && py < y + height;
        }
        [[nodiscard]] static constexpr Rectangle FromBounds(f32 left, f32 top, f32 right,
                                                            f32 bottom)
        {
            return Rectangle{left, top, right - left, bottom - top};
        }
    };

    // Per-glyph metrics.
    struct GlyphInfo
    {
        i32 codepoint = 0;
        i32 glyphIndex = 0; // 0 = missing glyph
        f32 advanceWidth = 0;
        f32 leftSideBearing = 0;
        Rectangle boundingBox; // in pixels, relative to baseline
        bool hasBitmap = false;
    };

    // UV + screen positioning for rendering a glyph from an atlas.
    struct GlyphQuad
    {
        f32 x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        f32 u0 = 0, v0 = 0, u1 = 0, v1 = 0;

        constexpr GlyphQuad() = default;
        constexpr GlyphQuad(f32 X0, f32 Y0, f32 X1, f32 Y1, f32 U0, f32 V0, f32 U1, f32 V1)
            : x0(X0), y0(Y0), x1(X1), y1(Y1), u0(U0), v0(V0), u1(U1), v1(V1)
        {
        }

        [[nodiscard]] constexpr f32 Width() const { return x1 - x0; }
        [[nodiscard]] constexpr f32 Height() const { return y1 - y0; }
    };

    // A positioned glyph in laid-out text.
    struct GlyphPosition
    {
        i32 stringIndex = 0;
        i32 codepoint = 0;
        f32 x = 0, y = 0;
        f32 advance = 0;
        GlyphInfo glyphInfo;
    };

    // A region within a font-atlas texture.
    struct AtlasRegion
    {
        u16 x = 0, y = 0, width = 0, height = 0;
        f32 offsetX = 0, offsetY = 0, advanceX = 0;

        constexpr AtlasRegion() = default;
        constexpr AtlasRegion(u16 x_, u16 y_, u16 w_, u16 h_, f32 ox, f32 oy, f32 adv)
            : x(x_), y(y_), width(w_), height(h_), offsetX(ox), offsetY(oy), advanceX(adv)
        {
        }

        void GetUVs(u32 atlasWidth, u32 atlasHeight, f32& u0, f32& v0, f32& u1, f32& v1) const
        {
            u0 = static_cast<f32>(x) / atlasWidth;
            v0 = static_cast<f32>(y) / atlasHeight;
            u1 = static_cast<f32>(x + width) / atlasWidth;
            v1 = static_cast<f32>(y + height) / atlasHeight;
        }
        [[nodiscard]] bool IsEmpty() const { return width == 0 || height == 0; }
    };

    // Metrics for text decorations (underline, strikethrough).
    struct TextDecorationMetrics
    {
        f32 underlinePosition = 0;
        f32 underlineThickness = 1;
        f32 strikethroughPosition = 0;
        f32 strikethroughThickness = 1;

        constexpr TextDecorationMetrics() = default;
        constexpr TextDecorationMetrics(f32 underPos, f32 underThick, f32 strikePos,
                                        f32 strikeThick)
            : underlinePosition(underPos), underlineThickness(underThick),
              strikethroughPosition(strikePos), strikethroughThickness(strikeThick)
        {
        }

        [[nodiscard]] static TextDecorationMetrics FromFontMetrics(f32 ascent, f32 pixelHeight)
        {
            const f32 underlinePos = pixelHeight * 0.12f;
            const f32 thickness = Max(1.0f, pixelHeight * 0.05f);
            const f32 strikePos = -ascent * 0.35f;
            return TextDecorationMetrics{underlinePos, thickness, strikePos, thickness};
        }
    };

    // Metrics for an entire font at a specific size.
    struct FontMetrics
    {
        f32 ascent = 0;
        f32 descent = 0;
        f32 lineGap = 0;
        f32 lineHeight = 0;
        f32 pixelHeight = 0;
        f32 scale = 1.0f;
        TextDecorationMetrics decorations;

        constexpr FontMetrics() = default;
        FontMetrics(f32 a, f32 d, f32 lg, f32 ph, f32 s)
            : ascent(a), descent(d), lineGap(lg), lineHeight(a - d + lg), pixelHeight(ph), scale(s),
              decorations(TextDecorationMetrics::FromFontMetrics(a, ph))
        {
        }
        FontMetrics(f32 a, f32 d, f32 lg, f32 ph, f32 s, TextDecorationMetrics dec)
            : ascent(a), descent(d), lineGap(lg), lineHeight(a - d + lg), pixelHeight(ph), scale(s),
              decorations(dec)
        {
        }

        [[nodiscard]] static FontMetrics Default() { return FontMetrics{0, 0, 0, 0, 1.0f}; }
    };

    // A text selection range [start, end).
    struct SelectionRange
    {
        i32 start = 0;
        i32 end = 0;

        constexpr SelectionRange() = default;
        SelectionRange(i32 s, i32 e) : start(Min(s, e)), end(Max(s, e)) {}

        [[nodiscard]] bool IsEmpty() const { return start == end; }
        [[nodiscard]] i32 Length() const { return end - start; }
        [[nodiscard]] bool Contains(i32 index) const { return index >= start && index < end; }
        [[nodiscard]] static SelectionRange FromAnchorActive(i32 anchor, i32 active)
        {
            return SelectionRange{Min(anchor, active), Max(anchor, active)};
        }
    };

    // Result of a hit test on text.
    struct HitTestResult
    {
        i32 characterIndex = 0;
        bool isTrailingHit = false;
        bool isInside = false;
        i32 lineIndex = 0;

        constexpr HitTestResult() = default;
        constexpr HitTestResult(i32 charIndex, bool trailing, bool inside, i32 line = 0)
            : characterIndex(charIndex), isTrailingHit(trailing), isInside(inside), lineIndex(line)
        {
        }

        [[nodiscard]] i32 InsertionIndex() const
        {
            return isTrailingHit ? characterIndex + 1 : characterIndex;
        }
    };

    // How an atlas stores its glyphs: 8-bit coverage (the classic rasterized alpha) or a
    // multi-channel signed distance field (MSDF) for resolution-independent, crisp scaling.
    enum class AtlasMode : u8
    {
        Coverage,
        DistanceField
    };

    // Options for loading a font.
    struct FontLoadOptions
    {
        f32 pixelHeight = 32.0f;
        i32 firstCodepoint = 32; // space
        i32 lastCodepoint = 126; // tilde
        u32 atlasWidth = 512;
        u32 atlasHeight = 512;
        u8 oversampleX = 2;
        u8 oversampleY = 2;
        u8 padding = 2;
        AtlasMode atlasMode = AtlasMode::Coverage;

        [[nodiscard]] i32 CharacterCount() const { return lastCodepoint - firstCodepoint + 1; }

        [[nodiscard]] static FontLoadOptions Default() { return FontLoadOptions{}; }
        [[nodiscard]] static FontLoadOptions ExtendedLatin()
        {
            FontLoadOptions o;
            o.lastCodepoint = 255;
            o.atlasWidth = 1024;
            o.atlasHeight = 1024;
            return o;
        }
        [[nodiscard]] static FontLoadOptions Small()
        {
            FontLoadOptions o;
            o.pixelHeight = 16.0f;
            o.atlasWidth = 256;
            o.atlasHeight = 256;
            return o;
        }
        [[nodiscard]] static FontLoadOptions Large()
        {
            FontLoadOptions o;
            o.pixelHeight = 64.0f;
            o.atlasWidth = 1024;
            o.atlasHeight = 1024;
            return o;
        }
        // An MSDF distance-field atlas: no oversampling (the field itself is analytic), a wider
        // padding to fit the distance spread. Baked at one size, sampled crisp at any scale.
        [[nodiscard]] static FontLoadOptions DistanceField()
        {
            FontLoadOptions o;
            o.atlasMode = AtlasMode::DistanceField;
            o.oversampleX = 1;
            o.oversampleY = 1;
            o.padding = 4;
            return o;
        }
    };

    enum class FontLoadResult
    {
        Success,
        FileNotFound,
        InvalidFormat,
        UnsupportedFormat,
        CorruptedData,
        OutOfMemory,
        NoGlyphsFound,
        AtlasPackingFailed,
        Unknown,
    };

    enum class TextAlignment
    {
        Left,
        Center,
        Right
    };
    enum class VerticalAlignment
    {
        Top,
        Middle,
        Bottom,
        Baseline
    };

    // Key for font cache lookups (family/path + pixel height).
    struct FontCacheKey
    {
        String path;
        f32 pixelHeight = 0;

        FontCacheKey() = default;
        FontCacheKey(StringView p, f32 ph) : path(p), pixelHeight(ph) {}

        [[nodiscard]] bool Equals(const FontCacheKey& other) const
        {
            return path == other.path && Abs(pixelHeight - other.pixelHeight) < 0.001f;
        }
    };

    [[nodiscard]] inline bool operator==(const FontCacheKey& a, const FontCacheKey& b)
    {
        return a.Equals(b);
    }
    [[nodiscard]] inline bool operator!=(const FontCacheKey& a, const FontCacheKey& b)
    {
        return !a.Equals(b);
    }
}

export namespace draconic::foundation
{
    // Hash for FontCacheKey, mirroring Sedulous: path hash combined with the
    // pixel height quantized to hundredths. Equality (operator==) uses a 0.001
    // tolerance - fine for the discrete font sizes a cache ever sees.
    template <>
    struct Hash<draconic::fonts::FontCacheKey>
    {
        [[nodiscard]] u64 operator()(const draconic::fonts::FontCacheKey& key) const noexcept
        {
            const u64 pathHash = Hash<String>{}(key.path);
            const u64 heightBucket = static_cast<u64>(static_cast<i64>(key.pixelHeight * 100.0f));
            return pathHash * 31u + heightBucket;
        }
    };
}
