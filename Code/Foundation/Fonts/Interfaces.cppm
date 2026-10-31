// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Fonts - :interfaces partition
//
// Abstract font interfaces (IFont, IFontAtlas, ITextShaper, IFontService) and
// the CachedFont aggregate. Backends (TTF, baked)
// implement these. Interfaces carry virtual destructors so CachedFont can own
// and delete them.

module;
#include "Core/Prelude.h"

export module foundation.fonts:interfaces;

import foundation.core;
import foundation.image;
import :types;

using namespace foundation::core;

export namespace foundation::fonts
{
    // Font data and metrics.
    class IFont
    {
    public:
        virtual ~IFont() = default;

        // Backend type tag for no-RTTI downcasts (-fno-rtti): a baker that must
        // recover its parser's concrete font type checks this instead of using
        // `dynamic_cast`/`is`. 0 = unspecified; backends return a unique id.
        [[nodiscard]] virtual u32 BackendTypeId() const { return 0; }

        [[nodiscard]] virtual StringView FamilyName() const = 0;
        /// The font file's bytes (TTF/OTF), for a shaper that reads its tables; empty when the
        /// font has none (a hand-built table).
        [[nodiscard]] virtual Span<const u8> SourceData() const = 0;
        [[nodiscard]] virtual FontMetrics Metrics() const = 0;
        [[nodiscard]] virtual f32 PixelHeight() const = 0;

        [[nodiscard]] virtual GlyphInfo GetGlyphInfo(i32 codepoint) const = 0;
        [[nodiscard]] virtual f32 GetKerning(i32 firstCodepoint, i32 secondCodepoint) const = 0;
        [[nodiscard]] virtual bool HasGlyph(i32 codepoint) const = 0;

        [[nodiscard]] virtual f32 MeasureString(StringView text) const = 0;
        [[nodiscard]] virtual f32 MeasureString(StringView text,
                                                Array<GlyphPosition>& outPositions) const = 0;
    };

    // Texture of pre-rendered glyphs.
    class IFontAtlas
    {
    public:
        virtual ~IFontAtlas() = default;

        [[nodiscard]] virtual u32 Width() const = 0;
        [[nodiscard]] virtual u32 Height() const = 0;
        [[nodiscard]] virtual Span<const u8> PixelData() const = 0; // single-channel 8-bit coverage

        // Glyphs are looked up by glyph id (what a shaper writes into GlyphPosition::glyphId). Text
        // drawn without a shaper maps each codepoint to its glyph with GlyphFor first.
        /// The glyph this atlas draws `codepoint` with, or -1 when it has none.
        [[nodiscard]] virtual i32 GlyphFor(i32 codepoint) const = 0;
        [[nodiscard]] virtual bool TryGetRegion(i32 glyphId, AtlasRegion& region) const = 0;
        [[nodiscard]] virtual bool GetGlyphQuad(i32 glyphId, f32& cursorX, f32 cursorY,
                                                GlyphQuad& quad) const = 0;
        [[nodiscard]] virtual bool GetGlyphQuadAt(i32 glyphId, f32 x, f32 y,
                                                  GlyphQuad& quad) const = 0;
        [[nodiscard]] virtual bool Contains(i32 glyphId) const = 0;
        [[nodiscard]] virtual Float2 WhitePixelUV() const = 0; // UV of a solid white texel

        // Storage mode and the distance-field spread (in texels) for MSDF atlases. They default
        // to a plain coverage atlas, so a coverage backend need not override them.
        [[nodiscard]] virtual AtlasMode Mode() const { return AtlasMode::Coverage; }
        [[nodiscard]] virtual f32 DistanceFieldRange() const { return 0.0f; }
    };

    /// What to shape: text in a list of fonts (a character takes the first font that has its glyph;
    /// one no font has takes the first), read in a direction, as a script and a language. The
    /// shaper splits the text into runs where the font or the script changes.
    struct ShapeRequest
    {
        StringView text;
        Span<IFont* const> fonts;
        TextDirection direction = TextDirection::Auto;
        /// An ISO 15924 script tag ('Arab', 'Latn', ...), 0 to find each run's from its characters.
        u32 script = 0;
        /// A BCP 47 language ("sr", "ar"), empty for none: a font may draw some characters as that
        /// language writes them (a Serbian б).
        StringView language;
        f32 startX = 0.0f;
        f32 startY = 0.0f;
    };

    /// ISO 15924 script tag from its four letters ("Arab" -> 'Arab').
    [[nodiscard]] constexpr u32 ScriptTag(const char (&letters)[5]) noexcept
    {
        return (static_cast<u32>(static_cast<u8>(letters[0])) << 24) |
               (static_cast<u32>(static_cast<u8>(letters[1])) << 16) |
               (static_cast<u32>(static_cast<u8>(letters[2])) << 8) | static_cast<u32>(static_cast<u8>(letters[3]));
    }

    // Text shaping and layout, and UI helpers.
    class ITextShaper
    {
    public:
        virtual ~ITextShaper() = default;

        /// Shape a request on one line: glyphs in drawing order from (startX, startY), each with
        /// its cluster, its glyph id and the font of the list it belongs to. Returns the advance.
        [[nodiscard]] virtual Result<f32> Shape(const ShapeRequest& request, Array<GlyphPosition>& outPositions) = 0;

        [[nodiscard]] virtual Result<f32> ShapeText(IFont& font, StringView text,
                                                    Array<GlyphPosition>& outPositions) = 0;
        [[nodiscard]] virtual Result<f32> ShapeText(IFont& font, StringView text, f32 startX,
                                                    f32 startY,
                                                    Array<GlyphPosition>& outPositions) = 0;
        [[nodiscard]] virtual Status ShapeTextWrapped(IFont& font, StringView text, f32 maxWidth,
                                                      Array<GlyphPosition>& outPositions,
                                                      f32& outTotalHeight) = 0;

        [[nodiscard]] virtual HitTestResult
        HitTest(IFont& font, Span<const GlyphPosition> positions, f32 x, f32 y) = 0;
        [[nodiscard]] virtual HitTestResult HitTestWrapped(IFont& font,
                                                           Span<const GlyphPosition> positions,
                                                           f32 x, f32 y, f32 lineHeight) = 0;
        [[nodiscard]] virtual f32
        GetCursorPosition(IFont& font, Span<const GlyphPosition> positions, i32 characterIndex) = 0;
        virtual void GetSelectionRects(IFont& font, Span<const GlyphPosition> positions,
                                       SelectionRange selection, f32 lineHeight,
                                       Array<Rectangle>& outRects) = 0;

        /// The glyphs shaping text of `codepoints` can produce beyond those the codepoints map to
        /// (ligatures, joined and contextual forms), as glyph indices: what a baker adds to an
        /// atlas so the shaped text draws. None for a shaper that maps one character to one glyph.
        virtual void CollectReachableGlyphs(const IFont& font, Span<const i32> codepoints,
                                            Array<i32>& outGlyphs) = 0;
    };

    // Owning aggregate of a loaded font, its atlas and an optional shaper.
    class CachedFont
    {
    public:
        IFont* font = nullptr;
        IFontAtlas* atlas = nullptr;
        ITextShaper* shaper = nullptr;
        i32 refCount = 1;

        // `allocator` is the one font, atlas and shaper were allocated from (the parse
        // or bake caller's decision); the aggregate frees them through it.
        CachedFont(IAllocator& allocator, IFont* f, IFontAtlas* a,
                   ITextShaper* s = nullptr) noexcept
            : font(f), atlas(a), shaper(s), m_allocator(&allocator)
        {
        }

        ~CachedFont()
        {
            if (shaper != nullptr)
            {
                m_allocator->Delete(shaper);
            }
            if (atlas != nullptr)
            {
                m_allocator->Delete(atlas);
            }
            if (font != nullptr)
            {
                m_allocator->Delete(font);
            }
        }

        CachedFont(const CachedFont&) = delete;
        CachedFont& operator=(const CachedFont&) = delete;

        [[nodiscard]] IAllocator& MemoryAllocator() const noexcept { return *m_allocator; }

    private:
        IAllocator* m_allocator;
    };

    // Provides fonts (loading/caching/atlas textures) to drawing/UI systems.
    class IFontService
    {
    public:
        virtual ~IFontService() = default;

        [[nodiscard]] virtual CachedFont* GetFont(f32 pixelHeight) = 0;
        [[nodiscard]] virtual CachedFont* GetFont(StringView familyName, f32 pixelHeight) = 0;
        [[nodiscard]] virtual foundation::image::ImageData* GetAtlasTexture(CachedFont* font) = 0;
        [[nodiscard]] virtual foundation::image::ImageData* GetAtlasTexture(StringView familyName,
                                                                          f32 pixelHeight) = 0;
        virtual void ReleaseFont(CachedFont* font) = 0;
        [[nodiscard]] virtual StringView DefaultFontFamily() const = 0;
        /// Whether a family by this name is loaded (GetFont falls back to the default family for
        /// one that is not, so asking it cannot tell).
        [[nodiscard]] virtual bool HasFamily(StringView familyName) const = 0;
    };
}
