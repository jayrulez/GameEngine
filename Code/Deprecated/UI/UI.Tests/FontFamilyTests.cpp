// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Font-service wiring: View::ResolveStyleFontFamily() falling back to the default of UIContext's
// IFontService, the FontFamily cascade winning over it, and per-instance overrides. A
// StubFontService returns null CachedFonts but a known default family; these unit tests never use
// a real font.
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
    /// Minimal stub returning a known default family name, so the tests can confirm that
    /// ResolveStyleFontFamily() falls back to the active IFontService when nothing else applies.
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

    // Give ctx a fresh empty stylesheet. Returns a borrowed pointer.
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
    // With no cascade rule and no inline override, it falls through to the font service's DefaultFontFamily.
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
    // An empty per-instance override defers to the cascade.
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
    // The VG resolves CachedFont atlases through its own service pointer (set at construction),
    // and a game swaps the UI context's service after the VG exists (SetDefaultFont binds the
    // cooked font once the project loads). DrawRootView re-asserts the context's service on the
    // VG; otherwise the VG would ask the stale service for atlases of CachedFonts it never
    // created, get null, and silently draw no text.
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get());

    StubFontService constructionService;
    StubFontService swappedService;
    foundation::vg::VGContext vgContext(&constructionService);
    ctx.SetFontService(&swappedService); // the swap happens after the VG was built

    ctx.DrawRootView(root.Get(), vgContext);

    CHECK(vgContext.FontService() == &swappedService); // the draw re-asserted the context's service
}
