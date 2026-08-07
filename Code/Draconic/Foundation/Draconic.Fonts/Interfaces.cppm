// Draconic::Fonts - :interfaces partition
//
// Abstract font interfaces (IFont, IFontAtlas, ITextShaper, IFontService) and
// the CachedFont aggregate. Ported from Sedulous.Fonts. Backends (TTF, baked)
// implement these. Interfaces carry virtual destructors so CachedFont can own
// and delete them.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.fonts:interfaces;

import draconic.foundation;
import draconic.image;
import :types;

using namespace draconic::foundation;

export namespace draconic::fonts
{
    // Font data + metrics.
    class IFont
    {
    public:
        virtual ~IFont() = default;

        // Backend type tag for no-RTTI downcasts (-fno-rtti): a baker that must
        // recover its parser's concrete font type checks this instead of using
        // `dynamic_cast`/`is`. 0 = unspecified; backends return a unique id.
        [[nodiscard]] virtual u32 BackendTypeId() const { return 0; }

        [[nodiscard]] virtual StringView FamilyName() const = 0;
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

        [[nodiscard]] virtual bool TryGetRegion(i32 codepoint, AtlasRegion& region) const = 0;
        [[nodiscard]] virtual bool GetGlyphQuad(i32 codepoint, f32& cursorX, f32 cursorY,
                                                GlyphQuad& quad) const = 0;
        [[nodiscard]] virtual bool GetGlyphQuadAt(i32 codepoint, f32 x, f32 y,
                                                  GlyphQuad& quad) const = 0;
        [[nodiscard]] virtual bool Contains(i32 codepoint) const = 0;
        [[nodiscard]] virtual Float2 WhitePixelUV() const = 0; // UV of a solid white texel

        // Storage mode + the distance-field spread (in texels) for MSDF atlases. Default to a
        // plain coverage atlas so every existing atlas keeps compiling unchanged.
        [[nodiscard]] virtual AtlasMode Mode() const { return AtlasMode::Coverage; }
        [[nodiscard]] virtual f32 DistanceFieldRange() const { return 0.0f; }
    };

    // Text shaping/layout + UI helpers.
    class ITextShaper
    {
    public:
        virtual ~ITextShaper() = default;

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
    };

    // Owning aggregate of a loaded font + its atlas (+ optional shaper).
    class CachedFont
    {
    public:
        IFont* font = nullptr;
        IFontAtlas* atlas = nullptr;
        ITextShaper* shaper = nullptr;
        i32 refCount = 1;

        CachedFont(IFont* f, IFontAtlas* a, ITextShaper* s = nullptr) noexcept
            : font(f), atlas(a), shaper(s)
        {
        }

        ~CachedFont()
        {
            if (shaper != nullptr)
            {
                DefaultAllocator().Delete(shaper);
            }
            if (atlas != nullptr)
            {
                DefaultAllocator().Delete(atlas);
            }
            if (font != nullptr)
            {
                DefaultAllocator().Delete(font);
            }
        }

        CachedFont(const CachedFont&) = delete;
        CachedFont& operator=(const CachedFont&) = delete;
    };

    // Provides fonts (loading/caching/atlas textures) to drawing/UI systems.
    class IFontService
    {
    public:
        virtual ~IFontService() = default;

        [[nodiscard]] virtual CachedFont* GetFont(f32 pixelHeight) = 0;
        [[nodiscard]] virtual CachedFont* GetFont(StringView familyName, f32 pixelHeight) = 0;
        [[nodiscard]] virtual draconic::image::ImageData* GetAtlasTexture(CachedFont* font) = 0;
        [[nodiscard]] virtual draconic::image::ImageData* GetAtlasTexture(StringView familyName,
                                                                          f32 pixelHeight) = 0;
        virtual void ReleaseFont(CachedFont* font) = 0;
        [[nodiscard]] virtual StringView DefaultFontFamily() const = 0;
    };
}
