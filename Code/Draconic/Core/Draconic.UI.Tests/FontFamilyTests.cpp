// Ported from Sedulous.UI.Tests/src/InlineStyleTests.bf - the ResolveStyleFontFamily / FontService
// subset (the tests that exercise the font-service wiring: View::ResolveStyleFontFamily() falling back
// to UIContext's IFontService default, the .FontFamily cascade winning over it, and per-instance
// overrides). Faithful to Sedulous: a StubFontService returns null CachedFonts but a known default
// family (Sedulous unit tests never use a real font - real glyph rendering is a sample concern).
// Beef `sheet.ForType(typeof(TestView))` -> ForType(&TestView::StaticType()); `ctx.FontService = x`
// -> ctx.SetFontService(&x).
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"
import draconic.foundation;
import draconic.ui;
import draconic.fonts;
import draconic.image; // ImageData (StubFontService::GetAtlasTexture return type)
#include "TestHelpers.h"

using namespace draconic::ui;
using namespace draconic::ui::tests;
using namespace draconic::foundation;
namespace foundation = draconic::foundation;
namespace fonts = draconic::fonts;

namespace
{
    /// Minimal stub returning a known default family name (Sedulous StubFontService). Confirms
    /// ResolveStyleFontFamily() floor-falls-back through the active IFontService.
    class StubFontService final : public fonts::IFontService
    {
    public:
        [[nodiscard]] fonts::CachedFont* GetFont(f32) override { return nullptr; }
        [[nodiscard]] fonts::CachedFont* GetFont(StringView, f32) override { return nullptr; }
        [[nodiscard]] draconic::image::ImageData* GetAtlasTexture(fonts::CachedFont*) override
        {
            return nullptr;
        }
        [[nodiscard]] draconic::image::ImageData* GetAtlasTexture(StringView, f32) override
        {
            return nullptr;
        }
        void ReleaseFont(fonts::CachedFont*) override {}
        [[nodiscard]] StringView DefaultFontFamily() const override { return u8"StubDefault"; }
    };

    foundation::RefPtr<RootView> MakeRoot() { return foundation::MakeRef<RootView>(foundation::DefaultAllocator()); }

    // Give ctx a fresh empty stylesheet (Sedulous SetupSheet). Returns a borrowed pointer.
    StyleSheet* SetupSheet(UIContext& ctx)
    {
        foundation::RefPtr<StyleSheet> sheet = foundation::MakeRef<StyleSheet>(foundation::DefaultAllocator());
        StyleSheet* raw = sheet.Get();
        ctx.SetStyleSheet(Move(sheet));
        return raw;
    }
}

TEST_CASE("font-family: ResolveStyleFontFamily_Fallback_UsesFontServiceDefault")
{
    // No cascade rule + no inline override -> falls through to the font service's DefaultFontFamily.
    UIContext ctx;
    auto root = MakeRoot();
    Init(ctx, root.Get());
    StubFontService fontService;
    ctx.SetFontService(&fontService);
    SetupSheet(ctx);

    auto view = foundation::MakeRef<TestView>(foundation::DefaultAllocator());
    root->AddView(view.Get());

    CHECK(view->ResolveStyleFontFamily() == u8"StubDefault");
}

TEST_CASE("font-family: ResolveStyleFontFamily_CascadeWinsOverFontServiceDefault")
{
    UIContext ctx;
    auto root = MakeRoot();
    Init(ctx, root.Get());
    StubFontService fontService;
    ctx.SetFontService(&fontService);

    StyleSheet* sheet = SetupSheet(ctx);
    sheet->ForType(&TestView::StaticType()).Set(StyleProperty::FontFamily, StringView{u8"Roboto"});

    auto view = foundation::MakeRef<TestView>(foundation::DefaultAllocator());
    root->AddView(view.Get());

    CHECK(view->ResolveStyleFontFamily() == u8"Roboto");
}

TEST_CASE("font-family: ResolveStyleFontFamily_InstanceOverride_Wins")
{
    UIContext ctx;
    auto root = MakeRoot();
    Init(ctx, root.Get());
    StyleSheet* sheet = SetupSheet(ctx);
    sheet->ForType(&TestView::StaticType()).Set(StyleProperty::FontFamily, StringView{u8"Roboto"});

    auto view = foundation::MakeRef<TestView>(foundation::DefaultAllocator());
    root->AddView(view.Get());

    CHECK(view->ResolveStyleFontFamily(StringView{u8"CustomFamily"}) == u8"CustomFamily");
}

TEST_CASE("font-family: ResolveStyleFontFamily_EmptyOverride_DefersToCascade")
{
    // Beef's null override -> our empty StringView: an empty per-instance override defers to the cascade.
    UIContext ctx;
    auto root = MakeRoot();
    Init(ctx, root.Get());
    StyleSheet* sheet = SetupSheet(ctx);
    sheet->ForType(&TestView::StaticType()).Set(StyleProperty::FontFamily, StringView{u8"Roboto"});

    auto view = foundation::MakeRef<TestView>(foundation::DefaultAllocator());
    root->AddView(view.Get());

    CHECK(view->ResolveStyleFontFamily(StringView{}) == u8"Roboto");
}

TEST_CASE("font-family: Resolution_InlineFontFamilyBeatsContextSheet")
{
    UIContext ctx;
    auto root = MakeRoot();
    Init(ctx, root.Get());
    StyleSheet* sheet = SetupSheet(ctx);
    sheet->ForType(&TestView::StaticType()).Set(StyleProperty::FontFamily, StringView{u8"Roboto"});

    auto view = foundation::MakeRef<TestView>(foundation::DefaultAllocator());
    root->AddView(view.Get());
    view->SetStyle(StyleProperty::FontFamily, StringView{u8"JungleAdventurer"});

    // Inline override beats the context sheet cascade.
    const StyleValue resolved = view->ResolveStyle(StyleProperty::FontFamily);
    CHECK(resolved.AsString().HasValue());
    CHECK(resolved.AsString().Value() == u8"JungleAdventurer");
}
