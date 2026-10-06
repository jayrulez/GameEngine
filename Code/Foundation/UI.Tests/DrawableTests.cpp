// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Ported from Sedulous.UI.Tests/src/DrawableTests.bf (faithful; Beef `new X()/defer ReleaseRef` ->
// stack values / MakeRef children, `===` reference-equality -> pointer ==).
// NOTE: the two NineSlice_* tests are not ported; they require NineSliceDrawable.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import foundation.image;
import foundation.vg;
import foundation.ui;

using namespace foundation::ui;
namespace core = foundation::core;

// === StateListDrawable ===

TEST_CASE("drawable: StateList_GetFallsBackToNormal")
{
    StateListDrawable sl;
    sl.Set(ControlState::Normal,
           core::MakeRef<ColorDrawable>(core::DefaultAllocator(), core::Color::Red));
    Drawable* normal = sl.Get(ControlState::Normal);

    CHECK(sl.Get(ControlState::Normal) == normal);
    CHECK(sl.Get(ControlState::Hover) == normal);    // fallback
    CHECK(sl.Get(ControlState::Pressed) == normal);  // fallback
    CHECK(sl.Get(ControlState::Disabled) == normal); // fallback
}

TEST_CASE("drawable: StateList_GetReturnsSpecificState")
{
    StateListDrawable sl;
    sl.Set(ControlState::Normal,
           core::MakeRef<ColorDrawable>(core::DefaultAllocator(), core::Color::Red));
    sl.Set(ControlState::Hover,
           core::MakeRef<ColorDrawable>(core::DefaultAllocator(), core::Color::Blue));
    Drawable* normal = sl.Get(ControlState::Normal);
    Drawable* hover = sl.Get(ControlState::Hover);

    CHECK(sl.Get(ControlState::Normal) == normal);
    CHECK(sl.Get(ControlState::Hover) == hover);
    CHECK(sl.Get(ControlState::Pressed) == normal); // fallback
}

TEST_CASE("drawable: StateList_GetReturnsNullIfNoNormal")
{
    StateListDrawable sl;
    CHECK(sl.Get(ControlState::Normal) == nullptr);
    CHECK(sl.Get(ControlState::Hover) == nullptr);
}

// === LayerDrawable ===

TEST_CASE("drawable: Layer_AddLayer_IncreasesCount")
{
    // Just verify it doesn't crash - drawing needs a VGContext. AddLayer consumes the ref.
    LayerDrawable layer;
    layer.AddLayer(core::MakeRef<ColorDrawable>(core::DefaultAllocator(), core::Color::Red));
    layer.AddLayer(core::MakeRef<ColorDrawable>(core::DefaultAllocator(), core::Color::Blue),
                   Thickness{5.0f, 5.0f, 5.0f, 5.0f});
}

// === InsetDrawable ===

TEST_CASE("drawable: Inset_DrawablePadding_MatchesInset")
{
    InsetDrawable inset{core::MakeRef<ColorDrawable>(core::DefaultAllocator(), core::Color::Red),
                        Thickness{10.0f, 5.0f, 10.0f, 5.0f}};
    Thickness pad = inset.DrawablePadding();
    CHECK(pad.Left == 10.0f);
    CHECK(pad.Top == 5.0f);
    CHECK(pad.Right == 10.0f);
    CHECK(pad.Bottom == 5.0f);
}

// === ColorDrawable ===

TEST_CASE("drawable: ColorDrawable_NoIntrinsicSize")
{
    ColorDrawable cd{core::Color::Red};
    CHECK_FALSE(cd.IntrinsicSize().HasValue());
}

// === RoundedRectDrawable ===

TEST_CASE("drawable: RoundedRect_NoIntrinsicSize")
{
    RoundedRectDrawable rr{core::Color::Red, 4.0f, core::Color::Blue, 1.0f};
    CHECK_FALSE(rr.IntrinsicSize().HasValue());
}

// === NineSliceDrawable ===

TEST_CASE("drawable: NineSlice_DrawablePadding_AccountsForExpand")
{
    NineSliceDrawable ns{nullptr, foundation::image::NineSlice{10.0f, 10.0f, 10.0f, 10.0f}};
    ns.Expand = Thickness{5.0f, 5.0f, 5.0f, 5.0f};
    Thickness pad = ns.DrawablePadding();
    // Padding = max(0, Slices - Expand) = max(0, 10-5) = 5
    CHECK(pad.Left == 5.0f);
    CHECK(pad.Top == 5.0f);
    CHECK(pad.Right == 5.0f);
    CHECK(pad.Bottom == 5.0f);
}

TEST_CASE("drawable: NineSlice_DrawablePadding_ClampsToZero")
{
    NineSliceDrawable ns{nullptr, foundation::image::NineSlice{5.0f, 5.0f, 5.0f, 5.0f}};
    ns.Expand = Thickness{10.0f, 10.0f, 10.0f, 10.0f};
    Thickness pad = ns.DrawablePadding();
    CHECK(pad.Left == 0.0f);
    CHECK(pad.Top == 0.0f);
}

// === Drawable base ===

TEST_CASE("drawable: Drawable_StateAwareDraw_DelegatesToStateless")
{
    // ShapeDrawable has no state-aware override - should delegate. Creation must not invoke it.
    bool called = false;
    ShapeDrawable sd{
        ShapeDrawable::DrawFn{[&](UIDrawContext&, const core::Rectangle&) { called = true; }}};
    CHECK_FALSE(called);
}

TEST_CASE("drawable: statelist Disabled dominates interaction flags")
{
    // A disabled control under the mouse is Disabled|Hover: it must render DISABLED, not
    // light up with the hover layer (the generic high->low flag stripping got this wrong).
    auto list = core::MakeRef<StateListDrawable>(core::DefaultAllocator());
    auto normal = core::MakeRef<ColorDrawable>(core::DefaultAllocator(), core::Color::Black);
    auto hover = core::MakeRef<ColorDrawable>(core::DefaultAllocator(), core::Color::Green);
    auto disabled = core::MakeRef<ColorDrawable>(core::DefaultAllocator(), core::Color::Red);
    list->Set(ControlState::Normal, core::RefPtr<Drawable>(normal.Get()));
    list->Set(ControlState::Hover, core::RefPtr<Drawable>(hover.Get()));
    list->Set(ControlState::Disabled, core::RefPtr<Drawable>(disabled.Get()));

    CHECK(list->Get(ControlState::Disabled | ControlState::Hover) == disabled.Get());
    CHECK(list->Get(ControlState::Disabled | ControlState::Pressed) == disabled.Get());
    CHECK(list->Get(ControlState::Disabled | ControlState::Focused | ControlState::Hover) ==
          disabled.Get());
    CHECK(list->Get(ControlState::Hover) == hover.Get()); // unchanged
    CHECK(list->Get(ControlState::Hover | ControlState::Focused) ==
          hover.Get()); // generic fallback intact
}

namespace
{
    // Records the rectangle it was last drawn into.
    class ProbeDrawable final : public Drawable
    {
    public:
        core::Rectangle lastBounds{};
        void Draw(UIDrawContext&, const core::Rectangle& bounds) override { lastBounds = bounds; }
    };
}

// KeepAspect draws at the desired shape, as large as fits and centred: a 14 by 14 icon in a 40 by
// 10 box is 10 by 10 in the middle, not 40 by 10. Without it the drawable fills (Sedulous eeaa5376).
TEST_CASE("drawable: DrawableView KeepAspect fits and centres rather than stretching")
{
    foundation::vg::VGContext vgContext;
    UIDrawContext draw(vgContext, 1.0f, nullptr);
    auto icon = core::MakeRef<ProbeDrawable>(core::DefaultAllocator());
    auto view = core::MakeRef<DrawableView>(core::DefaultAllocator(),
                                            DrawablePtr(icon.Get()), 14.0f, 14.0f);
    view->Measure(BoxConstraints::Tight(40, 10));
    view->Layout(0, 0, 40, 10);

    view->OnDraw(draw);
    CHECK(icon->lastBounds.width == doctest::Approx(40.0f)); // fills by default
    CHECK(icon->lastBounds.height == doctest::Approx(10.0f));

    view->KeepAspect = true;
    view->OnDraw(draw);
    CHECK(icon->lastBounds.width == doctest::Approx(10.0f));
    CHECK(icon->lastBounds.height == doctest::Approx(10.0f));
    CHECK(icon->lastBounds.x == doctest::Approx(15.0f)); // centred
    CHECK(icon->lastBounds.y == doctest::Approx(0.0f));
}

// A rounded picture is cut to its rounded rect (more than the plain quad's four vertices, all on
// the image's texture), the ImageView and the image() drawable alike; square corners stay the
// plain quad.
TEST_CASE("drawable: an ImageView and an ImageDrawable round the picture's corners")
{
    foundation::image::ImageDataRef picture{64, 32};
    auto drawOnce = [&](auto&& draw) -> core::usize
    {
        foundation::vg::VGContext vgContext;
        UIDrawContext ctx(vgContext, 1.0f, nullptr);
        draw(ctx);
        const foundation::vg::VGBatch& batch = vgContext.GetBatch();
        REQUIRE(batch.textures.Size() == 2u);
        CHECK(batch.textures[1] == &picture);
        return batch.VertexCount();
    };

    auto view = core::MakeRef<ImageView>(core::DefaultAllocator(), &picture);
    view->ScaleType.SetValue(ScaleType::FillBounds);
    view->Measure(BoxConstraints::Tight(128, 64));
    view->Layout(0, 0, 128, 64);
    View* asView = view.Get(); // OnDraw is public on View
    CHECK(drawOnce([&](UIDrawContext& ctx) { asView->OnDraw(ctx); }) == 4u);
    view->CornerRadius.SetValue(foundation::vg::CornerRadii(8.0f, 8.0f, 0.0f, 0.0f));
    CHECK(drawOnce([&](UIDrawContext& ctx) { asView->OnDraw(ctx); }) > 4u);

    ImageDrawable image(&picture);
    const core::Rectangle bounds{0, 0, 128, 64};
    CHECK(drawOnce([&](UIDrawContext& ctx) { image.Draw(ctx, bounds); }) == 4u);
    image.Radii = foundation::vg::CornerRadii(6.0f);
    CHECK(drawOnce([&](UIDrawContext& ctx) { image.Draw(ctx, bounds); }) > 4u);
}
