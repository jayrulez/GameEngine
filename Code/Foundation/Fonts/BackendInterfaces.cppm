// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Fonts - foundation.fonts:backend_interfaces partition (parser, atlas baker and
// atlas cache: the interfaces a font backend implements)
//
// The source-format load pipeline contracts: IFontParser (source bytes -> a
// queryable IFont) and IFontAtlasBaker (a parsed IFont -> a renderable
// IFontAtlas). Baked `.font` resources skip this pipeline entirely; they are
// pre-completed (IFont, IFontAtlas) pairs.

module;
#include "Core/Prelude.h"

export module foundation.fonts:backend_interfaces;

import foundation.core;
import :types;
import :interfaces;

using namespace foundation::core;

export namespace foundation::fonts
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

        // Canonical entry point: parse from a borrowed stream (not retained). The
        // returned font is allocated from `allocator`; the caller owns it and must
        // free it through the same allocator (CachedFont records it).
        [[nodiscard]] virtual Result<IFont*, FontLoadResult>
        ParseFromStream(IStream& stream, FontLoadOptions options, IAllocator& allocator) = 0;

        // Parse from an in-memory byte span.
        [[nodiscard]] virtual Result<IFont*, FontLoadResult>
        ParseFromMemory(Span<const u8> data, FontLoadOptions options, IAllocator& allocator) = 0;

        // Parse from a file on disk. Engine and packaged-game callers should prefer
        // the VFS-aware stream path.
        [[nodiscard]] virtual Result<IFont*, FontLoadResult>
        ParseFromFile(StringView filePath, FontLoadOptions options, IAllocator& allocator) = 0;
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

        // Produce a new atlas for the font and options, allocated from `allocator`.
        // Caller takes ownership and frees through the same allocator.
        [[nodiscard]] virtual Result<IFontAtlas*, FontLoadResult>
        Bake(IFont& font, FontLoadOptions options, IAllocator& allocator) = 0;
    };

    // Optional bake-result cache consulted by FontAtlasBakerFactory::Bake: a hit skips the
    // bake entirely; a miss bakes and offers the fresh atlas back for storage. Installed by
    // the application (SetAtlasCache); the policy (cache directory, keying, eviction) lives
    // with the installer, never here. An implementation may decline any font/options pair it
    // does not understand by returning null from TryLoad and ignoring Store.
    class IFontAtlasCache
    {
    public:
        virtual ~IFontAtlasCache() = default;

        // A cached atlas for this font and options (allocated from `allocator`, caller owns),
        // or null on miss/decline.
        [[nodiscard]] virtual IFontAtlas* TryLoad(const IFont& font,
                                                  const FontLoadOptions& options,
                                                  IAllocator& allocator) = 0;

        // Offer a freshly baked atlas for storage. Failures are the cache's to handle; the
        // bake result is returned to the caller regardless.
        virtual void Store(const IFont& font, const FontLoadOptions& options,
                           const IFontAtlas& atlas) = 0;
    };
}
