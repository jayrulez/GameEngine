// Raptor::FontsIO — raptor.fonts.io:factories partition
//
// Process-global registries + dispatchers for IFontParser / IFontAtlasBaker.
// Extension-based routing: each parser/baker declares the extensions it
// handles and the factory selects the match at call time. Backends register
// themselves at startup (e.g. TrueTypeFonts::Initialize). The registries own
// the registered objects and delete any still-registered on Shutdown; an
// explicit Unregister transfers that responsibility back to the caller.
//
// Ported from Sedulous.Fonts.IO (FontParserFactory.bf, FontAtlasBakerFactory.bf),
// keeping the static-class call style backed by function-local storage.

module;
#include "Core/Prelude.h"

export module raptor.fonts.io:factories;

import raptor.core;
import raptor.fonts;
import :interfaces;

using namespace raptor::core;

namespace raptor::fonts
{
    // Shared process-global storage (function-local statics so the lists live
    // for the program's lifetime and are shared across translation units).
    Array<IFontParser*>& ParserStore()
    {
        static Array<IFontParser*> parsers;
        return parsers;
    }

    Array<IFontAtlasBaker*>& BakerStore()
    {
        static Array<IFontAtlasBaker*> bakers;
        return bakers;
    }
}

export namespace raptor::fonts
{
    // Registry + dispatcher for IFontParser implementations.
    class FontParserFactory
    {
    public:
        static void RegisterParser(IFontParser* parser)
        {
            if (parser == nullptr)
                return;
            for (IFontParser* p : ParserStore())
                if (p == parser)
                    return;
            ParserStore().PushBack(parser);
        }

        static void UnregisterParser(IFontParser* parser)
        {
            Array<IFontParser*>& store = ParserStore();
            for (usize i = 0; i < store.Size(); ++i)
                if (store[i] == parser)
                {
                    store.RemoveAt(i);
                    return;
                }
        }

        // First parser claiming the given extension, or null.
        [[nodiscard]] static IFontParser* GetParserForExtension(StringView fileExtension)
        {
            for (IFontParser* p : ParserStore())
                if (p->SupportsExtension(fileExtension))
                    return p;
            return nullptr;
        }

        [[nodiscard]] static Result<IFont*, FontLoadResult> ParseFromFile(StringView filePath, FontLoadOptions options = FontLoadOptions::Default())
        {
            IFontParser* parser = GetParserForExtension(PathExtension(filePath));
            if (parser == nullptr)
                return Err(FontLoadResult::UnsupportedFormat);
            return parser->ParseFromFile(filePath, options);
        }

        [[nodiscard]] static Result<IFont*, FontLoadResult> ParseFromMemory(Span<const u8> data, StringView formatHint, FontLoadOptions options = FontLoadOptions::Default())
        {
            IFontParser* parser = GetParserForExtension(formatHint);
            if (parser == nullptr)
                return Err(FontLoadResult::UnsupportedFormat);
            return parser->ParseFromMemory(data, options);
        }

        [[nodiscard]] static Result<IFont*, FontLoadResult> ParseFromStream(IStream& stream, StringView formatHint, FontLoadOptions options = FontLoadOptions::Default())
        {
            IFontParser* parser = GetParserForExtension(formatHint);
            if (parser == nullptr)
                return Err(FontLoadResult::UnsupportedFormat);
            return parser->ParseFromStream(stream, options);
        }

        [[nodiscard]] static usize ParserCount() { return ParserStore().Size(); }
        [[nodiscard]] static bool HasParsers() { return ParserStore().Size() > 0; }

        // Clear the registry and delete the parsers it still owns.
        static void Shutdown()
        {
            Array<IFontParser*>& store = ParserStore();
            for (IFontParser* p : store)
                DefaultAllocator().Delete(p);
            store.Clear();
        }
    };

    // Registry + dispatcher for IFontAtlasBaker implementations.
    class FontAtlasBakerFactory
    {
    public:
        static void RegisterBaker(IFontAtlasBaker* baker)
        {
            if (baker == nullptr)
                return;
            for (IFontAtlasBaker* b : BakerStore())
                if (b == baker)
                    return;
            BakerStore().PushBack(baker);
        }

        static void UnregisterBaker(IFontAtlasBaker* baker)
        {
            Array<IFontAtlasBaker*>& store = BakerStore();
            for (usize i = 0; i < store.Size(); ++i)
                if (store[i] == baker)
                {
                    store.RemoveAt(i);
                    return;
                }
        }

        // First baker claiming the given extension, or null.
        [[nodiscard]] static IFontAtlasBaker* GetBakerForExtension(StringView fileExtension)
        {
            for (IFontAtlasBaker* b : BakerStore())
                if (b->SupportsExtension(fileExtension))
                    return b;
            return nullptr;
        }

        // First baker reporting CanBake(font) == true, or null.
        [[nodiscard]] static IFontAtlasBaker* GetBakerForFont(const IFont& font)
        {
            for (IFontAtlasBaker* b : BakerStore())
                if (b->CanBake(font))
                    return b;
            return nullptr;
        }

        // First baker reporting CanBake(font, options) == true, or null.
        [[nodiscard]] static IFontAtlasBaker* GetBakerForFont(const IFont& font, const FontLoadOptions& options)
        {
            for (IFontAtlasBaker* b : BakerStore())
                if (b->CanBake(font, options))
                    return b;
            return nullptr;
        }

        [[nodiscard]] static Result<IFontAtlas*, FontLoadResult> Bake(IFont& font, FontLoadOptions options = FontLoadOptions::Default())
        {
            IFontAtlasBaker* baker = GetBakerForFont(font, options);
            if (baker == nullptr)
                return Err(FontLoadResult::UnsupportedFormat);
            return baker->Bake(font, options);
        }

        [[nodiscard]] static Result<IFontAtlas*, FontLoadResult> BakeFromExtension(StringView fileExtension, IFont& font, FontLoadOptions options = FontLoadOptions::Default())
        {
            IFontAtlasBaker* baker = GetBakerForExtension(fileExtension);
            if (baker == nullptr)
                return Err(FontLoadResult::UnsupportedFormat);
            return baker->Bake(font, options);
        }

        [[nodiscard]] static usize BakerCount() { return BakerStore().Size(); }
        [[nodiscard]] static bool HasBakers() { return BakerStore().Size() > 0; }

        // Clear the registry and delete the bakers it still owns.
        static void Shutdown()
        {
            Array<IFontAtlasBaker*>& store = BakerStore();
            for (IFontAtlasBaker* b : store)
                DefaultAllocator().Delete(b);
            store.Clear();
        }
    };
}
