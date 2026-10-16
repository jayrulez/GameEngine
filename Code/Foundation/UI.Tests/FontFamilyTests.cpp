// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Ported from Sedulous.UI.Tests/src/InlineStyleTests.bf - the ResolveStyleFontFamily / FontService
// subset (the tests that exercise the font-service wiring: View::ResolveStyleFontFamily() falling back
// to UIContext's IFontService default, the .FontFamily cascade winning over it, and per-instance
// overrides). Faithful to Sedulous: a StubFontService returns null CachedFonts but a known default
// family (Sedulous unit tests never use a real font - real glyph rendering is a sample concern).
// Beef `sheet.ForType(typeof(TestView))` -> ForType(&TestView::StaticType()); `ctx.FontService = x`
// -> ctx.SetFontService(&x).
#include <doctest/doctest.h>
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"
import foundation.core;
import foundation.ui;
import foundation.fonts;
import foundation.image; // ImageData (StubFontService::GetAtlasTexture return type)
import foundation.vg;    // VGContext (the DrawRootView font-service push test)
#include "TestHelpers.h"

using namespace foundation::ui;
using namespace foundation::ui::tests;
using namespace foundation::core;
namespace core = foundation::core;
namespace fonts = foundation::fonts;

namespace
{
    /// Minimal stub returning a known default family name (Sedulous StubFontService). Confirms
    /// ResolveStyleFontFamily() floor-falls-back through the active IFontService.
    class StubFontService final : public fonts::IFontService
    {
    public:
        [[nodiscard]] fonts::CachedFont* GetFont(f32) override { return nullptr; }
        [[nodiscard]] fonts::CachedFont* GetFont(StringView, f32) override { return nullptr; }
        [[nodiscard]] foundation::image::ImageData* GetAtlasTexture(fonts::CachedFont*) override
        {
            return nullptr;
        }
        [[nodiscard]] foundation::image::ImageData* GetAtlasTexture(StringView, f32) override
        {
            return nullptr;
        }
        void ReleaseFont(fonts::CachedFont*) override {}
        [[nodiscard]] StringView DefaultFontFamily() const override { return u8"StubDefault"; }
        [[nodiscard]] bool HasFamily(StringView) const override { return false; }
    };

    core::RefPtr<RootView> MakeRoot() { return core::MakeRef<RootView>(core::DefaultAllocator()); }

    // Give ctx a fresh empty stylesheet (Sedulous SetupSheet). Returns a borrowed pointer.
    StyleSheet* SetupSheet(UIContext& ctx)
    {
        core::RefPtr<StyleSheet> sheet = core::MakeRef<StyleSheet>(core::DefaultAllocator());
        StyleSheet* raw = sheet.Get();
        ctx.SetStyleSheet(Move(sheet));
        return raw;
    }
}

TEST_CASE("font-family: ResolveStyleFontFamily_Fallback_UsesFontServiceDefault")
{
    // No cascade rule + no inline override -> falls through to the font service's DefaultFontFamily.
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get());
    StubFontService fontService;
    ctx.SetFontService(&fontService);
    SetupSheet(ctx);

    auto view = core::MakeRef<TestView>(core::DefaultAllocator());
    root->AddView(view.Get());

    CHECK(view->ResolveStyleFontFamily() == u8"StubDefault");
}

TEST_CASE("font-family: ResolveStyleFontFamily_CascadeWinsOverFontServiceDefault")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get());
    StubFontService fontService;
    ctx.SetFontService(&fontService);

    StyleSheet* sheet = SetupSheet(ctx);
    sheet->ForType(&TestView::StaticType()).Set(StyleProperty::FontFamily, StringView{u8"Roboto"});

    auto view = core::MakeRef<TestView>(core::DefaultAllocator());
    root->AddView(view.Get());

    CHECK(view->ResolveStyleFontFamily() == u8"Roboto");
}

TEST_CASE("font-family: ResolveStyleFontFamily_InstanceOverride_Wins")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get());
    StyleSheet* sheet = SetupSheet(ctx);
    sheet->ForType(&TestView::StaticType()).Set(StyleProperty::FontFamily, StringView{u8"Roboto"});

    auto view = core::MakeRef<TestView>(core::DefaultAllocator());
    root->AddView(view.Get());

    CHECK(view->ResolveStyleFontFamily(StringView{u8"CustomFamily"}) == u8"CustomFamily");
}

TEST_CASE("font-family: ResolveStyleFontFamily_EmptyOverride_DefersToCascade")
{
    // Beef's null override -> our empty StringView: an empty per-instance override defers to the cascade.
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get());
    StyleSheet* sheet = SetupSheet(ctx);
    sheet->ForType(&TestView::StaticType()).Set(StyleProperty::FontFamily, StringView{u8"Roboto"});

    auto view = core::MakeRef<TestView>(core::DefaultAllocator());
    root->AddView(view.Get());

    CHECK(view->ResolveStyleFontFamily(StringView{}) == u8"Roboto");
}

TEST_CASE("font-family: Resolution_InlineFontFamilyBeatsContextSheet")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get());
    StyleSheet* sheet = SetupSheet(ctx);
    sheet->ForType(&TestView::StaticType()).Set(StyleProperty::FontFamily, StringView{u8"Roboto"});

    auto view = core::MakeRef<TestView>(core::DefaultAllocator());
    root->AddView(view.Get());
    view->SetStyle(StyleProperty::FontFamily, StringView{u8"Chewy"});

    // Inline override beats the context sheet cascade.
    const StyleValue resolved = view->ResolveStyle(StyleProperty::FontFamily);
    CHECK(resolved.AsString().HasValue());
    CHECK(resolved.AsString().Value() == u8"Chewy");
}

TEST_CASE("font-service: DrawRootView pushes the context's CURRENT service into the VG")
{
    // The 2026-08-12 dist incident, pinned: the VG resolves CachedFont atlases through ITS OWN
    // service pointer (set at construction), and the game swaps the UI context's service AFTER
    // the VG exists (SetDefaultFont binding the cooked font once the project loads). If
    // DrawRootView does not re-assert the context's service, the VG asks the STALE service for
    // atlases of CachedFonts it never created - null, silent skip, invisible text in the
    // built game (masked in the source tree, where the stale service is the working one).
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get());

    StubFontService constructionService;
    StubFontService swappedService;
    foundation::vg::VGContext vgContext(&constructionService);
    ctx.SetFontService(&swappedService); // the swap happens after the VG was built

    ctx.DrawRootView(root.Get(), vgContext);

    CHECK(vgContext.FontService() == &swappedService); // the draw re-asserted the truth
}
