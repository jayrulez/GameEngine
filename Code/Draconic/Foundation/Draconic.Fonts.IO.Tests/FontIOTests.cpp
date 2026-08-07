// Validates the draconic.fonts.io port: extension-routed parser/baker factories
// and the FontManager cache. Sedulous exercised this path through its TTF
// backend; here we drive it with fake parser/baker built on the baked types so
// the IO layer is tested in isolation (no stb_truetype).
#include <doctest/doctest.h>

#include "Draconic.Foundation/Prelude.h"

import draconic.foundation;
import draconic.fonts;
import draconic.fonts.baked;
import draconic.fonts.io;

using namespace draconic::foundation;
using namespace draconic::fonts;

namespace
{
    // A parser that "parses" any input into a one-glyph BakedFont.
    class FakeParser final : public IFontParser
    {
    public:
        Span<const StringView> SupportedExtensions() const override
        {
            static const StringView exts[] = {StringView(u8".fake")};
            return Span<const StringView>(exts, 1);
        }
        bool SupportsExtension(StringView ext) const override
        {
            return ext == StringView(u8".fake");
        }

        Result<IFont*, FontLoadResult> ParseFromStream(IStream&, FontLoadOptions) override
        {
            return Make();
        }
        Result<IFont*, FontLoadResult> ParseFromMemory(Span<const u8>, FontLoadOptions) override
        {
            return Make();
        }
        Result<IFont*, FontLoadResult> ParseFromFile(StringView, FontLoadOptions) override
        {
            return Make();
        }

    private:
        static Result<IFont*, FontLoadResult> Make()
        {
            BakedFont* f = DefaultAllocator().New<BakedFont>();
            f->SetFamilyName(u8"Fake");
            GlyphInfo g;
            g.advanceWidth = 8.0f;
            f->SetGlyph(static_cast<i32>('A'), g);
            return static_cast<IFont*>(f);
        }
    };

    // A baker that bakes any BakedFont into an empty BakedFontAtlas.
    class FakeBaker final : public IFontAtlasBaker
    {
    public:
        Span<const StringView> SupportedExtensions() const override
        {
            static const StringView exts[] = {StringView(u8".fake")};
            return Span<const StringView>(exts, 1);
        }
        bool SupportsExtension(StringView ext) const override
        {
            return ext == StringView(u8".fake");
        }
        bool CanBake(const IFont&) const override { return true; }

        Result<IFontAtlas*, FontLoadResult> Bake(IFont&, FontLoadOptions) override
        {
            BakedFontAtlas* a = DefaultAllocator().New<BakedFontAtlas>();
            return static_cast<IFontAtlas*>(a);
        }
    };
}

TEST_CASE("io.factories: parser registration + extension dispatch")
{
    FontParserFactory::Shutdown(); // start clean
    CHECK_FALSE(FontParserFactory::HasParsers());

    FontParserFactory::RegisterParser(DefaultAllocator().New<FakeParser>());
    FontParserFactory::RegisterParser(nullptr); // ignored
    CHECK(FontParserFactory::ParserCount() == 1);

    CHECK(FontParserFactory::GetParserForExtension(u8".fake") != nullptr);
    CHECK(FontParserFactory::GetParserForExtension(u8".nope") == nullptr);

    // Unknown extension -> UnsupportedFormat.
    Result<IFont*, FontLoadResult> miss =
        FontParserFactory::ParseFromMemory(Span<const u8>(), u8".nope");
    CHECK_FALSE(miss.HasValue());
    CHECK(miss.Error() == FontLoadResult::UnsupportedFormat);

    // Known extension -> a parsed font (caller owns it).
    Result<IFont*, FontLoadResult> hit =
        FontParserFactory::ParseFromMemory(Span<const u8>(), u8".fake");
    REQUIRE(hit.HasValue());
    CHECK(hit.Value()->HasGlyph(static_cast<i32>('A')));
    DefaultAllocator().Delete(hit.Value());

    FontParserFactory::Shutdown();
    CHECK(FontParserFactory::ParserCount() == 0);
}

TEST_CASE("io.factories: baker dispatch by extension and by font")
{
    FontAtlasBakerFactory::Shutdown();
    FontAtlasBakerFactory::RegisterBaker(DefaultAllocator().New<FakeBaker>());
    CHECK(FontAtlasBakerFactory::BakerCount() == 1);

    BakedFont font;
    CHECK(FontAtlasBakerFactory::GetBakerForFont(font) != nullptr);

    Result<IFontAtlas*, FontLoadResult> byFont = FontAtlasBakerFactory::Bake(font);
    REQUIRE(byFont.HasValue());
    DefaultAllocator().Delete(byFont.Value());

    Result<IFontAtlas*, FontLoadResult> byExt =
        FontAtlasBakerFactory::BakeFromExtension(u8".fake", font);
    REQUIRE(byExt.HasValue());
    DefaultAllocator().Delete(byExt.Value());

    Result<IFontAtlas*, FontLoadResult> miss =
        FontAtlasBakerFactory::BakeFromExtension(u8".nope", font);
    CHECK_FALSE(miss.HasValue());

    FontAtlasBakerFactory::Shutdown();
}

TEST_CASE("io.manager: cache hit, refcount, clear")
{
    FontParserFactory::Shutdown();
    FontAtlasBakerFactory::Shutdown();
    FontParserFactory::RegisterParser(DefaultAllocator().New<FakeParser>());
    FontAtlasBakerFactory::RegisterBaker(DefaultAllocator().New<FakeBaker>());

    FontManager manager;
    CHECK(manager.CacheCount() == 0);
    CHECK_FALSE(manager.IsCached(u8"font.fake", 16));

    CachedFont* a = manager.GetFont(u8"font.fake", 16);
    REQUIRE(a != nullptr);
    CHECK(manager.CacheCount() == 1);
    CHECK(manager.IsCached(u8"font.fake", 16));
    CHECK(a->refCount == 1);

    // Same key -> same instance, refcount bumped.
    CachedFont* b = manager.GetFont(u8"font.fake", 16);
    CHECK(b == a);
    CHECK(a->refCount == 2);

    // Different size -> a distinct cached entry.
    CachedFont* c = manager.GetFont(u8"font.fake", 32);
    CHECK(c != a);
    CHECK(manager.CacheCount() == 2);

    // Release to zero refs, then ClearUnused evicts only the unreferenced one.
    manager.ReleaseFont(a);
    manager.ReleaseFont(a); // back to 0
    manager.ClearUnused();
    CHECK(manager.CacheCount() == 1); // only the 32px entry survives (refCount 1)

    manager.ClearAll();
    CHECK(manager.CacheCount() == 0);

    FontParserFactory::Shutdown();
    FontAtlasBakerFactory::Shutdown();
}
