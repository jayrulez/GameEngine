// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Style model v2, P2 (ui-layout-and-style-model.md section 2): the box model on every view.
// Draw order and hit order by z-index, Position::Absolute children in any container (insets,
// far-edge anchoring, both-edges pinning), box-shadow through the VG DF shadow mode, opacity
// composing down the tree, sheet-driven LayoutStyle fields (width/height/margin/min/max/
// position/z-index/overflow) with the inline-wins rule, and the markup attribute vocabulary.

#include <doctest/doctest.h>

#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

import foundation.core;
import foundation.vg;
import foundation.ui;

#include "TestHelpers.h"

using namespace foundation::ui;
using namespace foundation::ui::tests;
using namespace foundation::core;
namespace core = foundation::core;
namespace vg = foundation::vg;

namespace
{
    /// A TestView that records the order it was drawn in and paints an opaque rect (so the
    /// batch carries its composed opacity).
    class RecordingView final : public TestView
    {
        RTTI_OBJECT(RecordingView, TestView)
    public:
        static inline Array<RecordingView*> DrawLog{};
        i32 Tag = 0;

        RecordingView() = default;
        RecordingView(f32 w, f32 h, i32 tag) : TestView(w, h), Tag(tag) {}

        void OnDraw(UIDrawContext& ctx) override
        {
            DrawLog.PushBack(this);
            ctx.VG().FillRect(Rectangle{0, 0, Width(), Height()}, Color{1.0f, 0.0f, 0.0f, 1.0f});
        }
    };
    RTTI_DEFINE_OBJECT(RecordingView, "rtti::ui::tests")

    void EnsureGlobals()
    {
        StyleSheetLoader::InitializeGlobals();
        UITypeRegistry::Register(u8"View", &View::StaticType());
        UITypeRegistry::Register(u8"RootView", &RootView::StaticType());
        UITypeRegistry::Register(u8"TestView", &TestView::StaticType());
        UITypeRegistry::Register(u8"TestGroup", &TestGroup::StaticType());
        UITypeRegistry::Register(u8"RecordingView", &RecordingView::StaticType());
    }

    core::RefPtr<StyleSheet> LoadSSS(StringView src)
    {
        EnsureGlobals();
        StyleSheetLoader loader(DefaultAllocator());
        return loader.Load(src);
    }

    core::RefPtr<RootView> MakeRoot() { return core::MakeRef<RootView>(core::DefaultAllocator()); }
    core::RefPtr<TestView> TV(f32 w, f32 h) { return core::MakeRef<TestView>(core::DefaultAllocator(), w, h); }
    core::RefPtr<RecordingView> RV(f32 w, f32 h, i32 tag)
    {
        return core::MakeRef<RecordingView>(core::DefaultAllocator(), w, h, tag);
    }
    template <typename T>
    core::RefPtr<T> New()
    {
        return core::MakeRef<T>(core::DefaultAllocator());
    }

    [[nodiscard]] bool FindCommandWithMode(const vg::VGBatch& batch, vg::VGDrawMode mode,
                                           vg::VGCommand& out)
    {
        for (usize i = 0; i < batch.CommandCount(); ++i)
        {
            const vg::VGCommand cmd = batch.GetCommand(i);
            if (cmd.drawMode == mode && cmd.indexCount > 0)
            {
                out = cmd;
                return true;
            }
        }
        return false;
    }
}

// === z-index ===

TEST_CASE("box-model: z-index orders the draw back to front; ties keep child order")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto frame = New<FrameLayout>();
    auto a = RV(50, 30, 1);
    auto b = RV(50, 30, 2);
    auto c = RV(50, 30, 3);
    LayoutStyle front;
    front.ZIndex = 1;
    frame->AddView(a.Get(), front); // z 1, first child
    frame->AddView(b.Get());        // z 0
    frame->AddView(c.Get(), front); // z 1, third child
    root->AddView(frame.Get());
    LayoutPass(ctx, root.Get());

    vg::VGContext vgContext;
    RecordingView::DrawLog.Clear();
    ctx.DrawRootView(root.Get(), vgContext);

    REQUIRE(RecordingView::DrawLog.Size() == 3);
    CHECK(RecordingView::DrawLog[0]->Tag == 2); // z 0 first
    CHECK(RecordingView::DrawLog[1]->Tag == 1); // then the z-1 pair in child order
    CHECK(RecordingView::DrawLog[2]->Tag == 3);

    // Without any z-index the child order is untouched.
    a->SetLayout(LayoutStyle{});
    c->SetLayout(LayoutStyle{});
    LayoutPass(ctx, root.Get());
    RecordingView::DrawLog.Clear();
    ctx.DrawRootView(root.Get(), vgContext);
    REQUIRE(RecordingView::DrawLog.Size() == 3);
    CHECK(RecordingView::DrawLog[0]->Tag == 1);
    CHECK(RecordingView::DrawLog[2]->Tag == 3);
}

TEST_CASE("box-model: hit testing walks the z order front to back")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto frame = New<FrameLayout>();
    auto below = TV(100, 100);
    auto above = TV(100, 100);
    LayoutStyle front;
    front.ZIndex = 5;
    frame->AddView(above.Get(), front); // FIRST child but drawn on top
    frame->AddView(below.Get());
    root->AddView(frame.Get());
    LayoutPass(ctx, root.Get());

    CHECK(frame->HitTest(Float2{10, 10}) == above.Get());
    front.ZIndex = -5;
    above->SetLayout(front);
    CHECK(frame->HitTest(Float2{10, 10}) == below.Get());
}

// === Position::Absolute ===

TEST_CASE("box-model: an absolute child inside a Flex takes no flow space and lands on its insets")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto flex = New<FlexLayout>();
    auto a = TV(50, 30);
    auto b = TV(50, 30);
    auto badge = TV(20, 10);
    LayoutStyle abs;
    abs.Position = Position::Absolute;
    abs.Left = 10;
    abs.Top = 5;
    flex->AddView(a.Get());
    flex->AddView(badge.Get(), abs);
    flex->AddView(b.Get());
    auto host = New<FrameLayout>();
    host->AddView(flex.Get());
    root->AddView(host.Get());
    LayoutPass(ctx, root.Get());

    // The flow children pack as if the badge were not there ...
    CHECK(a->Bounds.x == doctest::Approx(0));
    CHECK(b->Bounds.x == doctest::Approx(50));
    CHECK(flex->MeasuredSize.x == doctest::Approx(100));
    // ... and the badge sits at its insets with its own size.
    CHECK(badge->Bounds.x == doctest::Approx(10));
    CHECK(badge->Bounds.y == doctest::Approx(5));
    CHECK(badge->Bounds.width == doctest::Approx(20));
    CHECK(badge->Bounds.height == doctest::Approx(10));
    CHECK_FALSE(View::IsInFlow(badge.Get()));
    CHECK(View::IsInFlow(a.Get()));
}

TEST_CASE("box-model: right/bottom anchor the far edges; left+right pin both edges")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto frame = New<FrameLayout>();
    LayoutStyle frameSize;
    frameSize.Width = SizeSpec::Fixed(Unit::Dp(200));
    frameSize.Height = SizeSpec::Fixed(Unit::Dp(100));
    frame->SetLayout(frameSize);
    frame->Padding = Thickness{10, 10, 10, 10};

    auto corner = TV(20, 10);
    LayoutStyle anchored;
    anchored.Position = Position::Absolute;
    anchored.Right = 4;
    anchored.Bottom = 6;
    frame->AddView(corner.Get(), anchored);

    auto bar = TV(20, 10);
    LayoutStyle pinned;
    pinned.Position = Position::Absolute;
    pinned.Left = 8;
    pinned.Right = 12;
    pinned.Top = 0;
    frame->AddView(bar.Get(), pinned);
    auto host = New<FrameLayout>();
    host->AddView(frame.Get());
    root->AddView(host.Get());
    LayoutPass(ctx, root.Get());

    // Content box: x 10..190 (180 wide), y 10..90 (80 tall).
    CHECK(corner->Bounds.x == doctest::Approx(10 + 180 - 4 - 20));
    CHECK(corner->Bounds.y == doctest::Approx(10 + 80 - 6 - 10));
    CHECK(bar->Bounds.x == doctest::Approx(10 + 8));
    CHECK(bar->Bounds.width == doctest::Approx(180 - 8 - 12));
    CHECK(bar->Bounds.y == doctest::Approx(10));
}

TEST_CASE("box-model: an absolute child does not grow a wrapping container")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto group = New<TestGroup>(); // the base ViewGroup measure/arrange
    auto content = TV(50, 30);
    auto floating = TV(300, 200);
    LayoutStyle abs;
    abs.Position = Position::Absolute;
    abs.Left = 5;
    abs.Top = 5;
    group->AddView(content.Get());
    group->AddView(floating.Get(), abs);
    auto host = New<FrameLayout>();
    host->AddView(group.Get());
    root->AddView(host.Get());
    LayoutPass(ctx, root.Get());
    CHECK(group->MeasuredSize.x == doctest::Approx(50));
    CHECK(group->MeasuredSize.y == doctest::Approx(30));
    CHECK(floating->Bounds.x == doctest::Approx(5));
    CHECK(floating->Bounds.width == doctest::Approx(50 - 5)); // loose within the content box
}

// === box-shadow ===

TEST_CASE("box-model: box-shadow emits the DF shadow quad under the child with the right bounds")
{
    // The shadow is the child's border box grown by the spread, offset, blurred: sigma =
    // blur / 2 and the quads reach 3 sigma past the rect.
    {
        UIContext ctx{DefaultAllocator()};
        auto root = MakeRoot();
        EnsureGlobals();
        Init(ctx, root.Get(), 400, 300);
        ctx.SetStyleSheet(LoadSSS(u8"RecordingView { box-shadow: 4 6 8 2 #00000080; corner-radius: 3; }"));
        auto frame = New<FrameLayout>();
        frame->Padding = Thickness{20, 10, 0, 0};
        auto child = RV(100, 50, 1);
        frame->AddView(child.Get());
        root->AddView(frame.Get());
        LayoutPass(ctx, root.Get());
        REQUIRE(child->Bounds.x == doctest::Approx(20));
        REQUIRE(child->Bounds.y == doctest::Approx(10));

        vg::VGContext vgContext;
        RecordingView::DrawLog.Clear();
        ctx.DrawRootView(root.Get(), vgContext);

        const vg::VGBatch& batch = vgContext.GetBatch();
        vg::VGCommand cmd;
        REQUIRE(FindCommandWithMode(batch, vg::VGDrawMode::BoxShadow, cmd));
        CHECK(cmd.indexCount == 24); // four quadrant quads

        f32 minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
        usize shadowVertices = 0;
        for (usize i = 0; i < batch.vertices.Size(); ++i)
        {
            const vg::VGVertex& v = batch.vertices[i];
            if (v.color.a > 0.49f && v.color.a < 0.51f)
            {
                ++shadowVertices;
                minX = Min(minX, v.position.x);
                maxX = Max(maxX, v.position.x);
                minY = Min(minY, v.position.y);
                maxY = Max(maxY, v.position.y);
                // radius + spread, in sigma units.
                CHECK(v.coverage == doctest::Approx((3.0f + 2.0f) / 4.0f));
            }
        }
        CHECK(shadowVertices == 16);
        // rect: x = 20 + 4 - 2 = 22, w = 104; y = 10 + 6 - 2 = 14, h = 54; extent 3 * 4 = 12.
        CHECK(minX == doctest::Approx(22 - 12));
        CHECK(maxX == doctest::Approx(22 + 104 + 12));
        CHECK(minY == doctest::Approx(14 - 12));
        CHECK(maxY == doctest::Approx(14 + 54 + 12));

        // The shadow is drawn BEFORE the child's own content (it sits underneath).
        bool shadowFirst = false;
        for (usize i = 0; i < batch.CommandCount(); ++i)
        {
            const vg::VGCommand c = batch.GetCommand(i);
            if (c.indexCount == 0)
            {
                continue;
            }
            shadowFirst = c.drawMode == vg::VGDrawMode::BoxShadow;
            break;
        }
        CHECK(shadowFirst);
    }
}

TEST_CASE("box-model: box-shadow parses none, the inset keyword and a bare color")
{
    core::RefPtr<StyleSheet> sheet = LoadSSS(u8"TestView { box-shadow: 1 2 #ff0000 inset; }"
                                             u8".none { box-shadow: none; }"
                                             u8".lead { box-shadow: inset 3 4 5; }");
    REQUIRE(sheet);
    REQUIRE(sheet->RuleCount() == 3);
    const Optional<BoxShadow> a = sheet->GetRule(0).GetValue(StyleProperty::BoxShadow).Value().AsShadow();
    REQUIRE(a.HasValue());
    CHECK(a.Value().OffsetX == doctest::Approx(1));
    CHECK(a.Value().OffsetY == doctest::Approx(2));
    CHECK(a.Value().Blur == doctest::Approx(0));
    CHECK(a.Value().Inset);
    CHECK(a.Value().Color.r == doctest::Approx(1.0f));
    CHECK_FALSE(sheet->GetRule(1).GetValue(StyleProperty::BoxShadow).HasValue());
    const Optional<BoxShadow> c = sheet->GetRule(2).GetValue(StyleProperty::BoxShadow).Value().AsShadow();
    REQUIRE(c.HasValue());
    CHECK(c.Value().Inset);
    CHECK(c.Value().Blur == doctest::Approx(5));
    CHECK(c.Value().Color.a == doctest::Approx(0.5f)); // the default shadow color
}

// === opacity ===

TEST_CASE("box-model: opacity composes down the tree")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto outer = New<FrameLayout>();
    outer->Opacity = 0.5f;
    auto inner = RV(50, 30, 1);
    inner->Opacity = 0.5f;
    outer->AddView(inner.Get());
    root->AddView(outer.Get());
    LayoutPass(ctx, root.Get());

    vg::VGContext vgContext;
    ctx.DrawRootView(root.Get(), vgContext);
    const vg::VGBatch& batch = vgContext.GetBatch();
    REQUIRE(batch.vertices.Size() >= 4);
    for (usize i = 0; i < batch.vertices.Size(); ++i)
    {
        CHECK(batch.vertices[i].color.a == doctest::Approx(0.25f));
    }
}

TEST_CASE("box-model: with layers, a faded view with children fades as one group; a faded leaf alone")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto outer = New<FrameLayout>();
    outer->Opacity = 0.5f;
    auto inner = RV(50, 30, 1);
    inner->Opacity = 0.5f;
    outer->AddView(inner.Get());
    root->AddView(outer.Get());
    LayoutPass(ctx, root.Get());

    vg::VGContext vgContext;
    vgContext.SetLayers(true);
    ctx.DrawRootView(root.Get(), vgContext);
    const vg::VGBatch& batch = vgContext.GetBatch();
    REQUIRE(batch.layers.Size() == 1u); // the frame's; the leaf fades its own fill
    CHECK(batch.layers[0].opacity == doctest::Approx(0.5f));
    bool sawFill = false;
    bool sawComposite = false;
    for (const vg::VGCommand& cmd : batch.commands)
    {
        const f32 alpha = batch.vertices[batch.indices[static_cast<usize>(cmd.startIndex)]].color.a;
        if (cmd.drawMode == vg::VGDrawMode::Layer)
        {
            sawComposite = true;
            CHECK(cmd.layer == -1);
            CHECK(alpha == doctest::Approx(0.5f)); // the group's opacity, once
        }
        else
        {
            sawFill = true;
            CHECK(cmd.layer == 0);
            CHECK(alpha == doctest::Approx(0.5f)); // the leaf's own, inside the layer
        }
    }
    CHECK(sawFill);
    CHECK(sawComposite);
}

// === sheet-driven LayoutStyle ===

TEST_CASE("box-model: the cascade fills undeclared LayoutStyle fields; inline wins")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    EnsureGlobals();
    Init(ctx, root.Get(), 400, 300);
    ctx.SetStyleSheet(LoadSSS(u8".wide { width: 120; height: 40; margin: 4; }"
                              u8".fill { width: match; }"
                              u8".percent { width: 50%; }"));
    auto frame = New<FrameLayout>();
    auto styled = TV(50, 30);
    styled->AddClass(u8"wide");
    auto inlineWins = TV(50, 30);
    inlineWins->AddClass(u8"wide");
    LayoutStyle inlineWidth;
    inlineWidth.Width = SizeSpec::Fixed(Unit::Dp(60)); // height stays the sheet's
    inlineWins->SetLayout(inlineWidth);
    auto fill = TV(50, 30);
    fill->AddClass(u8"fill");
    auto half = TV(50, 30);
    half->AddClass(u8"percent");
    frame->AddView(styled.Get());
    frame->AddView(inlineWins.Get());
    frame->AddView(fill.Get());
    frame->AddView(half.Get());
    root->AddView(frame.Get());
    LayoutPass(ctx, root.Get());

    CHECK(styled->Bounds.width == doctest::Approx(120));
    CHECK(styled->Bounds.height == doctest::Approx(40));
    CHECK(styled->Bounds.x == doctest::Approx(4)); // the sheet margin
    CHECK_FALSE(styled->DeclaredLayout().Width.IsDeclared());
    CHECK(styled->Layout().Width->kind == SizeSpec::Kind::Fixed);

    CHECK(inlineWins->Bounds.width == doctest::Approx(60));
    CHECK(inlineWins->Bounds.height == doctest::Approx(40));
    CHECK(inlineWins->DeclaredLayout().Width.IsDeclared());

    CHECK(fill->Layout().Width->kind == SizeSpec::Kind::Match);
    CHECK(fill->Bounds.width == doctest::Approx(400));
    CHECK(half->Bounds.width == doctest::Approx(200));

    // An inline value equal to the DEFAULT still wins over the sheet (declared != default).
    LayoutStyle wrap;
    wrap.Width = SizeSpec::Wrap();
    fill->SetLayout(wrap);
    LayoutPass(ctx, root.Get());
    CHECK(fill->Bounds.width == doctest::Approx(50));
}

TEST_CASE("box-model: min/max clamp the size from inline fields and from the sheet")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    EnsureGlobals();
    Init(ctx, root.Get(), 400, 300);
    ctx.SetStyleSheet(LoadSSS(u8".narrow { max-width: 40; }"
                              u8".tall { min-height: 3em; font-size: 20; }"));
    auto frame = New<FrameLayout>();
    auto wideMin = TV(50, 30);
    LayoutStyle ls;
    ls.MinWidth = Unit::Dp(80);
    ls.MaxHeight = Unit::Dp(20);
    frame->AddView(wideMin.Get(), ls);
    auto narrow = TV(50, 30);
    narrow->AddClass(u8"narrow");
    frame->AddView(narrow.Get());
    auto tall = TV(50, 30);
    tall->AddClass(u8"tall");
    frame->AddView(tall.Get());
    auto fixedClamped = TV(50, 30);
    LayoutStyle fixed;
    fixed.Width = SizeSpec::Fixed(Unit::Dp(100));
    fixed.MaxWidth = Unit::Percent(10); // of the 400 wide frame
    frame->AddView(fixedClamped.Get(), fixed);
    root->AddView(frame.Get());
    LayoutPass(ctx, root.Get());

    CHECK(wideMin->Bounds.width == doctest::Approx(80));
    CHECK(wideMin->Bounds.height == doctest::Approx(20));
    CHECK(narrow->Bounds.width == doctest::Approx(40));
    CHECK(tall->Bounds.height == doctest::Approx(60));
    CHECK(fixedClamped->Bounds.width == doctest::Approx(40));
}

TEST_CASE("box-model: position, z-index, insets and overflow come from the sheet too")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    EnsureGlobals();
    Init(ctx, root.Get(), 400, 300);
    ctx.SetStyleSheet(LoadSSS(u8".badge { position: absolute; right: 4; top: 2; z-index: 3; }"
                              u8".clip { overflow: hidden; }"
                              u8".grow { flex-grow: 1; align-self: end; }"));
    auto flex = New<FlexLayout>();
    LayoutStyle flexSize;
    flexSize.Width = SizeSpec::Fixed(Unit::Dp(300));
    flexSize.Height = SizeSpec::Fixed(Unit::Dp(100));
    flex->SetLayout(flexSize);
    auto a = TV(50, 30);
    auto badge = TV(20, 10);
    badge->AddClass(u8"badge");
    auto grow = TV(50, 30);
    grow->AddClass(u8"grow");
    auto clip = TV(50, 30);
    clip->AddClass(u8"clip");
    flex->AddView(a.Get());
    flex->AddView(badge.Get());
    flex->AddView(grow.Get());
    flex->AddView(clip.Get());
    auto host = New<FrameLayout>();
    host->AddView(flex.Get());
    root->AddView(host.Get());
    LayoutPass(ctx, root.Get());

    CHECK(badge->Layout().Position.Value() == Position::Absolute);
    CHECK(badge->Layout().ZIndex.Value() == 3);
    CHECK(badge->Bounds.x == doctest::Approx(300 - 4 - 20));
    CHECK(badge->Bounds.y == doctest::Approx(2));
    CHECK(grow->Layout().FlexGrow.Value() == doctest::Approx(1));
    REQUIRE(grow->Layout().AlignSelf.HasValue());
    CHECK(grow->Layout().AlignSelf.Value() == Align::End);
    CHECK(grow->Bounds.width == doctest::Approx(300 - 50 - 50)); // absorbed the free space
    CHECK(grow->Bounds.y == doctest::Approx(100 - 30));
    CHECK(clip->EffectiveClipsContent());
    CHECK_FALSE(clip->ClipsContent);
    CHECK_FALSE(a->EffectiveClipsContent());

    // Removing the class un-styles the field on the next pass (nothing sticks).
    clip->RemoveClass(u8"clip");
    badge->RemoveClass(u8"badge");
    LayoutPass(ctx, root.Get());
    CHECK_FALSE(clip->EffectiveClipsContent());
    CHECK(badge->Layout().Position.Value() == Position::Static);
    CHECK(View::IsInFlow(badge.Get()));
}

// === markup vocabulary ===

TEST_CASE("box-model: the markup layout attributes cover the P2 fields")
{
    LayoutStyle ls;
    CHECK(MarkupRegistry::ApplyLayoutAttribute(ls, u8"position", u8"absolute"));
    CHECK(MarkupRegistry::ApplyLayoutAttribute(ls, u8"right", u8"12"));
    CHECK(MarkupRegistry::ApplyLayoutAttribute(ls, u8"bottom", u8"7.5"));
    CHECK(MarkupRegistry::ApplyLayoutAttribute(ls, u8"z-index", u8"-2"));
    CHECK(MarkupRegistry::ApplyLayoutAttribute(ls, u8"min-width", u8"40"));
    CHECK(MarkupRegistry::ApplyLayoutAttribute(ls, u8"max-width", u8"50%"));
    CHECK(MarkupRegistry::ApplyLayoutAttribute(ls, u8"min-height", u8"2em"));
    CHECK(MarkupRegistry::ApplyLayoutAttribute(ls, u8"max-height", u8"calc(100% - 20)"));
    CHECK(ls.Position.Value() == Position::Absolute);
    CHECK(ls.Position.IsDeclared());
    CHECK(ls.Right.Value() == doctest::Approx(12));
    CHECK(ls.Bottom.Value() == doctest::Approx(7.5));
    CHECK(ls.ZIndex.Value() == -2);
    CHECK(ls.MinWidth->dp == doctest::Approx(40));
    CHECK(ls.MaxWidth->percent == doctest::Approx(50));
    CHECK(ls.MinHeight->em == doctest::Approx(2));
    CHECK(ls.MaxHeight->percent == doctest::Approx(100));
    CHECK(ls.MaxHeight->dp == doctest::Approx(-20));
    CHECK_FALSE(ls.Left.IsDeclared());

    // Every P2 name is in the one table the loader and the completion share.
    const Span<const StringView> names = MarkupRegistry::LayoutAttributeNames();
    const StringView expected[] = {u8"position",  u8"right",     u8"bottom",    u8"z-index",
                                   u8"min-width", u8"min-height", u8"max-width", u8"max-height"};
    for (const StringView& want : expected)
    {
        bool found = false;
        for (const StringView& name : names)
        {
            found = found || name == want;
        }
        CHECK_MESSAGE(found, "missing layout attribute");
    }
}

TEST_CASE("box-model: Declared fields distinguish set-to-default from unset")
{
    LayoutStyle a;
    LayoutStyle b;
    CHECK(a == b);
    b.Width = SizeSpec::Wrap(); // the default VALUE, but declared
    CHECK_FALSE(a == b);
    CHECK(b.Width.IsDeclared());
    CHECK(b.Width == SizeSpec::Wrap());
    b.Width.Clear(SizeSpec::Wrap());
    CHECK(a == b);
}

// === consumer migration ===

TEST_CASE("box-model: ResolveStyleFloat resolves a Length value (theme em reaches every control)")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    EnsureGlobals();
    Init(ctx, root.Get(), 400, 300);
    ctx.SetStyleSheet(LoadSSS(u8"TestView { font-size: 20; corner-radius: 0.5em; spacing: calc(2em + 4); }"));
    auto view = TV(50, 30);
    root->AddView(view.Get());
    LayoutPass(ctx, root.Get());
    CHECK(view->ResolveStyleFloat(StyleProperty::CornerRadius, -1.0f) == doctest::Approx(10));
    CHECK(view->ResolveStyleFloat(StyleProperty::Spacing, -1.0f) == doctest::Approx(44));
    CHECK(view->ResolveStyleFloat(StyleProperty::BorderWidth, -1.0f) == doctest::Approx(-1)); // unset
}
