// Draconic::FontsIO - draconic.fonts.io:interfaces partition
//
// The source-format load pipeline contracts: IFontParser (source bytes -> a
// queryable IFont) and IFontAtlasBaker (a parsed IFont -> a renderable
// IFontAtlas). Baked `.font` resources skip this pipeline entirely - they are
// pre-completed (IFont, IFontAtlas) pairs. Ported from Sedulous.Fonts.IO
// (IFontParser.bf, IFontAtlasBaker.bf).

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.fonts.io:interfaces;

import draconic.foundation;
import draconic.fonts;

using namespace draconic::foundation;

export namespace draconic::fonts
{
    // Parses a source-format font (TTF, OTF, ...) into a queryable IFont. The
    // returned font is heap-allocated; the caller takes ownership.
    class IFontParser
    {
    public:
        virtual ~IFontParser() = default;

        // File extensions this parser supports (e.g. ".ttf", ".otf"), used by
        // FontParserFactory for extension dispatch.
        [[nodiscard]] virtual Span<const StringView> SupportedExtensions() const = 0;

        // Quick predicate over the extension list.
        [[nodiscard]] virtual bool SupportsExtension(StringView fileExtension) const = 0;

        // Canonical entry point: parse from a borrowed stream (not retained).
        [[nodiscard]] virtual Result<IFont*, FontLoadResult>
        ParseFromStream(IStream& stream, FontLoadOptions options) = 0;

        // Parse from an in-memory byte span.
        [[nodiscard]] virtual Result<IFont*, FontLoadResult>
        ParseFromMemory(Span<const u8> data, FontLoadOptions options) = 0;

        // Parse from a file on disk. Engine/shipped-game callers should prefer
        // the VFS-aware stream path.
        [[nodiscard]] virtual Result<IFont*, FontLoadResult>
        ParseFromFile(StringView filePath, FontLoadOptions options) = 0;
    };

    // Bakes a parsed IFont into a renderable IFontAtlas. Implementations
    // typically require a specific concrete IFont type (CanBake tests it).
    class IFontAtlasBaker
    {
    public:
        virtual ~IFontAtlasBaker() = default;

        // File extensions this baker is paired with, for factory dispatch.
        [[nodiscard]] virtual Span<const StringView> SupportedExtensions() const = 0;

        // Quick predicate over the extension list.
        [[nodiscard]] virtual bool SupportsExtension(StringView fileExtension) const = 0;

        // True if this baker can produce an atlas from the given font instance.
        [[nodiscard]] virtual bool CanBake(const IFont& font) const = 0;

        // Options-aware variant: lets bakers that share a font type disambiguate by the requested
        // atlas mode (coverage vs distance field). Defaults to the type-only predicate.
        [[nodiscard]] virtual bool CanBake(const IFont& font, const FontLoadOptions& options) const
        {
            (void)options;
            return CanBake(font);
        }

        // Produce a new atlas for the font + options. Caller takes ownership.
        [[nodiscard]] virtual Result<IFontAtlas*, FontLoadResult> Bake(IFont& font,
                                                                       FontLoadOptions options) = 0;
    };
}
