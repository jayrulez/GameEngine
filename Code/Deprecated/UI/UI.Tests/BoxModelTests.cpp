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

TEST_CASE("box-model: a box-shadow follows the corners its background rounds")
{
    // Sky Hopper's cards round their background (rounded-rect(..., radius=24)) and set no
    // corner-radius; their shadows were square. The shadow's corners, in sigma units, ride every
    // shadow vertex's coverage: blur 8 is sigma 4.
    const auto shadowCoverage = [](StringView sheetText) -> f32
    {
        UIContext ctx{DefaultAllocator()};
        auto root = MakeRoot();
        EnsureGlobals();
        Init(ctx, root.Get(), 400, 300);
        ctx.SetStyleSheet(LoadSSS(sheetText));
        auto child = RV(100, 50, 1);
        root->AddView(child.Get());
        LayoutPass(ctx, root.Get());
        vg::VGContext vgContext;
        ctx.DrawRootView(root.Get(), vgContext);
        const vg::VGBatch& batch = vgContext.GetBatch();
        for (usize i = 0; i < batch.vertices.Size(); ++i)
        {
            const vg::VGVertex& v = batch.vertices[i];
            if (v.color.a > 0.49f && v.color.a < 0.51f)
            {
                return v.coverage;
            }
        }
        return -1.0f;
    };
    CHECK(shadowCoverage(u8"RecordingView { background: rounded-rect(#FFFFFF, radius=12); "
                         u8"box-shadow: 0 0 8 #00000080; }") == doctest::Approx(12.0f / 4.0f));
    // Its spread grows the corners as it grows the box.
    CHECK(shadowCoverage(u8"RecordingView { background: rounded-rect(#FFFFFF, radius=12); "
                         u8"box-shadow: 0 0 8 2 #00000080; }") == doctest::Approx(14.0f / 4.0f));
    // A corner-radius the view sets still decides.
    CHECK(shadowCoverage(u8"RecordingView { background: rounded-rect(#FFFFFF, radius=12); "
                         u8"corner-radius: 3; box-shadow: 0 0 8 #00000080; }") == doctest::Approx(3.0f / 4.0f));
    // A background that rounds nothing leaves the shadow square.
    CHECK(shadowCoverage(u8"RecordingView { background: #FFFFFF; box-shadow: 0 0 8 #00000080; }") ==
          doctest::Approx(0.0f));
}

TEST_CASE("box-model: a clipping view with rounded corners clips its children to them")
{
    // What a clipping card holds draws inside its corners: through a stencil clip of its rounded
    // box, over the rectangle clip every clipping view keeps. A square one stays a scissor.
    const auto stencilClippedDraws = [](StringView sheetText) -> usize
    {
        UIContext ctx{DefaultAllocator()};
        auto root = MakeRoot();
        EnsureGlobals();
        Init(ctx, root.Get(), 400, 300);
        ctx.SetStyleSheet(LoadSSS(sheetText));
        auto card = New<FrameLayout>();
        card->AddClass(u8"card");
        card->ClipsContent = true;
        auto content = RV(100, 50, 1);
        card->AddView(content.Get());
        root->AddView(card.Get());
        LayoutPass(ctx, root.Get());
        vg::VGContext vgContext;
        vgContext.SetStencilFills(true);
        ctx.DrawRootView(root.Get(), vgContext);
        const vg::VGBatch& batch = vgContext.GetBatch();
        usize clipped = 0;
        for (usize i = 0; i < batch.CommandCount(); ++i)
        {
            const vg::VGCommand c = batch.GetCommand(i);
            if (c.indexCount > 0 && c.clipMode == vg::VGClipMode::Stencil &&
                c.fillPhase == vg::VGFillPhase::Direct)
            {
                ++clipped;
            }
        }
        return clipped;
    };
    CHECK(stencilClippedDraws(u8".card { corner-radius: 8; }") > 0u);
    // The corners a background rounds clip as they shape the shadow.
    CHECK(stencilClippedDraws(u8".card { background: rounded-rect(#FFFFFF, radius=12); }") > 0u);
    CHECK(stencilClippedDraws(u8".card { background: #FFFFFF; }") == 0u);
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

TEST_CASE("box-model: box-shadow takes a list, each drawn, the first on top")
{
    core::RefPtr<StyleSheet> sheet =
        LoadSSS(u8"RecordingView { box-shadow: 0 2 4 rgba(255, 0, 0, 0.5), inset 1 1 2 #0000FF80, 0 8 16 #00FF0080; }");
    REQUIRE(sheet);
    // Held: GetValue returns a copy, and the span reads into it.
    const Optional<StyleValue> value = sheet->GetRule(0).GetValue(StyleProperty::BoxShadow);
    REQUIRE(value.HasValue());
    const Span<const BoxShadow> shadows = value.Value().AsShadows();
    REQUIRE(shadows.Size() == 3u);
    CHECK(shadows[0].Color.r == doctest::Approx(1.0f)); // the rgba()'s commas are its own
    CHECK(shadows[1].Inset);
    CHECK(shadows[2].Blur == doctest::Approx(16));

    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    EnsureGlobals();
    Init(ctx, root.Get(), 400, 300);
    ctx.SetStyleSheet(sheet);
    auto child = RV(100, 50, 1);
    root->AddView(child.Get());
    LayoutPass(ctx, root.Get());
    vg::VGContext vgContext;
    ctx.DrawRootView(root.Get(), vgContext);
    const vg::VGBatch& batch = vgContext.GetBatch();
    // The outer two under the content, the green (last) one first so the red is on top of it;
    // the inset one over the content.
    Array<char> order;
    bool contentSeen = false;
    for (usize i = 0; i + 3 < batch.vertices.Size(); i += 4)
    {
        const vg::VGVertex& v = batch.vertices[i];
        const bool shadow = v.color.a > 0.49f && v.color.a < 0.51f;
        const char tag = !shadow ? 'c' : v.color.r > 0.99f ? 'r' : v.color.g > 0.99f ? 'g' : 'b';
        if (tag == 'c' && contentSeen)
            continue;
        contentSeen = contentSeen || tag == 'c';
        if (order.IsEmpty() || order.Back() != tag)
            order.PushBack(tag);
    }
    REQUIRE(order.Size() == 4u);
    CHECK(order[0] == 'g');
    CHECK(order[1] == 'r');
    CHECK(order[2] == 'c');
    CHECK(order[3] == 'b');
}

// === device-pixel rounding ===

TEST_CASE("box-model: a box under a scaled view rounds to the device pixels it is drawn on")
{
    // A game canvas scaled to its reference resolution draws its document at a scale: at 0.5, a
    // box at 3 layout units would sit at 1.5 device pixels. It rounds to 4 (2 device pixels), and
    // the top at 5 (2.5) to 6.
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    Init(ctx, root.Get(), 400, 300);
    auto canvas = New<FrameLayout>();
    canvas->Transform.Scale = Float2{0.5f, 0.5f};
    canvas->Transform.Origin = Float2{0.0f, 0.0f};
    auto child = TV(21, 11);
    LayoutStyle placed;
    placed.Margin = Thickness{3, 5, 0, 0};
    child->SetLayout(placed);
    canvas->AddView(child.Get());
    root->AddView(canvas.Get());
    LayoutPass(ctx, root.Get());
    CHECK(child->Bounds.x == doctest::Approx(4.0f));
    CHECK(child->Bounds.y == doctest::Approx(6.0f));
    // Its far edges too, from where they fall: 5 + 11 = 16 is 8 device pixels, and 3 + 21 = 24 is
    // 12, so the box keeps its size.
    CHECK(child->Bounds.x + child->Bounds.width == doctest::Approx(24.0f));
    CHECK(child->Bounds.y + child->Bounds.height == doctest::Approx(16.0f));
    CHECK(child->DeviceUnitScale() == doctest::Approx(0.5f));

    // Unscaled, the DPI's grid alone.
    canvas->Transform.Scale = Float2{1.0f, 1.0f};
    child->Invalidate();
    LayoutPass(ctx, root.Get());
    CHECK(child->Bounds.x == doctest::Approx(3.0f));
}

// === transform ===

TEST_CASE("box-model: a sheet's transform moves, scales and turns a view as drawn and hit-tested")
{
    core::RefPtr<StyleSheet> sheet = LoadSSS(u8".lift { transform: translate(10, 0) scale(2); }"
                                             u8".turn { transform: rotate(0.25turn); }"
                                             u8".bad { transform: perspective(10); }");
    REQUIRE(sheet);
    const Optional<StyleTransform> lift = sheet->GetRule(0).GetValue(StyleProperty::Transform).Value().AsTransform();
    REQUIRE(lift.HasValue());
    REQUIRE(lift.Value().Count == 2u); // in the order written
    CHECK(lift.Value().Functions[0].Kind == TransformFunctionKind::Translate);
    CHECK(lift.Value().Functions[0].X.dp == doctest::Approx(10.0f));
    CHECK(lift.Value().Functions[1].Kind == TransformFunctionKind::Scale);
    CHECK(lift.Value().Functions[1].B == doctest::Approx(2.0f));
    CHECK(sheet->GetRule(1).GetValue(StyleProperty::Transform).Value().AsTransform().Value().Functions[0].A ==
          doctest::Approx(90.0f));
    CHECK_FALSE(sheet->GetRule(2).GetValue(StyleProperty::Transform).HasValue()); // a 3D function: not drawn

    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    EnsureGlobals();
    Init(ctx, root.Get(), 400, 300);
    ctx.SetStyleSheet(sheet);
    auto child = RV(100, 50, 1);
    child->AddClass(u8"lift");
    child->Transform.Translation = Float2{0.0f, 4.0f}; // the code's own transform stays under it
    auto frame = New<FrameLayout>();
    frame->Padding = Thickness{150, 100, 0, 0};
    frame->AddView(child.Get());
    root->AddView(frame.Get());
    LayoutPass(ctx, root.Get());
    const Rectangle box = child->Bounds; // layout keeps its box
    CHECK(box.width == doctest::Approx(100.0f));
    vg::VGContext vgContext;
    ctx.DrawRootView(root.Get(), vgContext);
    // About the centre: (0, 0) is (-50, -25) from it, doubled to (-100, -50), back by (50, 25),
    // then moved by (10, 0) and the field's (0, 4).
    const Float2 corner = child->LocalToParent(Float2{0.0f, 0.0f}); // in the frame's space
    CHECK(corner.x == doctest::Approx(box.x - 40.0f));
    CHECK(corner.y == doctest::Approx(box.y - 21.0f));
    const Float2 back = child->ParentToLocal(corner);
    CHECK(back.x == doctest::Approx(0.0f).epsilon(0.001));
    CHECK(back.y == doctest::Approx(0.0f).epsilon(0.001));
    // The drawn quad spans the scaled box.
    f32 minX = 1e9f, maxX = -1e9f;
    for (const vg::VGVertex& v : vgContext.GetBatch().vertices)
    {
        minX = Min(minX, v.position.x);
        maxX = Max(maxX, v.position.x);
    }
    CHECK(maxX - minX == doctest::Approx(200.0f));
    // Hit testing follows it: a point the scaled view covers outside its layout box.
    CHECK(root->HitTest(Float2{box.x - 30.0f, box.y + 10.0f}) == child.Get()); // frame at the root's origin
}

TEST_CASE("box-model: a transform composes in the order written, about its transform-origin")
{
    // Composed as CSS composes them: the last function written applies to the view first.
    const auto cornerOf = [](StringView sheetText) -> Float2
    {
        UIContext ctx{DefaultAllocator()};
        auto root = MakeRoot();
        EnsureGlobals();
        Init(ctx, root.Get(), 400, 300);
        ctx.SetStyleSheet(LoadSSS(sheetText));
        auto child = RV(100, 50, 1);
        child->AddClass(u8"t");
        auto frame = New<FrameLayout>();
        frame->AddView(child.Get());
        root->AddView(frame.Get());
        LayoutPass(ctx, root.Get());
        vg::VGContext vgContext;
        ctx.DrawRootView(root.Get(), vgContext);
        // The view's own top-right corner, in its box's space (Bounds at the origin here).
        return child->LocalToParent(Float2{100.0f, 0.0f});
    };
    // Turned a quarter about the top-left, then moved 20 along the turned x (now down).
    const Float2 a = cornerOf(u8".t { transform-origin: left top; transform: rotate(90deg) translateX(20); }");
    CHECK(a.x == doctest::Approx(0.0f).epsilon(0.001));
    CHECK(a.y == doctest::Approx(120.0f));
    // Moved 20 right first, then turned about the top-left: the move turns with it.
    const Float2 b = cornerOf(u8".t { transform-origin: left top; transform: translateX(20) rotate(90deg); }");
    CHECK(b.x == doctest::Approx(20.0f));
    CHECK(b.y == doctest::Approx(100.0f));
    // Scaled up from the bottom-right corner: that corner stays, the top-left moves away.
    const Float2 c = cornerOf(u8".t { transform-origin: right bottom; transform: scale(2); }");
    CHECK(c.x == doctest::Approx(100.0f));
    CHECK(c.y == doctest::Approx(-50.0f));
    // A percentage moves by the view's own size; a skew slants x by y.
    const Float2 d = cornerOf(u8".t { transform-origin: 0 0; transform: translate(50%, 10%) skewY(45deg); }");
    CHECK(d.x == doctest::Approx(150.0f));
    CHECK(d.y == doctest::Approx(105.0f)); // 5 down, then 100 along x slanted 45 degrees
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

TEST_CASE("box-model: the sheet's opacity fades a view, times its own")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    EnsureGlobals();
    Init(ctx, root.Get(), 400, 300);
    ctx.SetStyleSheet(LoadSSS(u8"RecordingView { opacity: 0.5; }"));
    auto view = RV(50, 30, 1);
    view->Opacity = 0.5f;
    root->AddView(view.Get());
    LayoutPass(ctx, root.Get());

    vg::VGContext vgContext;
    ctx.DrawRootView(root.Get(), vgContext);
    const vg::VGBatch& batch = vgContext.GetBatch();
    REQUIRE(batch.vertices.Size() >= 4);
    CHECK(batch.vertices[0].color.a == doctest::Approx(0.25f));
}

TEST_CASE("box-model: filter blurs a view through a layer; backdrop-filter blurs what is behind it")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    EnsureGlobals();
    Init(ctx, root.Get(), 400, 300);
    ctx.SetStyleSheet(LoadSSS(u8".soft { filter: blur(4px); } .glass { backdrop-filter: blur(6); corner-radius: 8; } "
                              u8".plain { filter: none; }"));
    auto under = RV(200, 200, 1); // something to be behind the glass
    auto soft = RV(50, 30, 2);
    soft->AddClass(u8"soft");
    auto glass = RV(80, 60, 3);
    glass->AddClass(u8"glass");
    auto plain = RV(20, 20, 4);
    plain->AddClass(u8"plain");
    auto frame = New<FrameLayout>();
    frame->AddView(under.Get());
    frame->AddView(soft.Get());
    frame->AddView(glass.Get());
    frame->AddView(plain.Get());
    root->AddView(frame.Get());
    LayoutPass(ctx, root.Get());

    vg::VGContext vgContext;
    vgContext.SetLayers(true);
    ctx.DrawRootView(root.Get(), vgContext);
    const vg::VGBatch& batch = vgContext.GetBatch();
    int blurred = 0;
    int backdrops = 0;
    for (const vg::VGLayer& layer : batch.layers)
    {
        if (layer.blur > 0.0f)
        {
            CHECK(layer.blur == doctest::Approx(4.0f)); // a leaf blurs too: the filter needs the layer
            ++blurred;
        }
        if (layer.backdropBlur > 0.0f)
        {
            CHECK(layer.backdropBlur == doctest::Approx(6.0f));
            ++backdrops;
        }
    }
    CHECK(blurred == 1);
    CHECK(backdrops == 1);
    CHECK(batch.layers.Size() == 2u); // `filter: none` makes none
}

TEST_CASE("box-model: filter greys and dims a view at its layer's composite")
{
    core::RefPtr<StyleSheet> sheet = LoadSSS(u8".off { filter: grayscale(100%) brightness(0.8) blur(2); }"
                                             u8".half { filter: grayscale(0.5) saturate(1.5); }"
                                             u8".odd { filter: sepia(1); }");
    REQUIRE(sheet);
    const StyleFilter off = sheet->GetRule(0).GetValue(StyleProperty::Filter).Value().AsFilter().Value();
    CHECK(off.Saturation == doctest::Approx(0.0f));
    CHECK(off.Brightness == doctest::Approx(0.8f));
    CHECK(off.Blur == doctest::Approx(2.0f));
    // The colour factors multiply: half grey, then half again as vivid.
    CHECK(sheet->GetRule(1).GetValue(StyleProperty::Filter).Value().AsFilter().Value().Saturation ==
          doctest::Approx(0.75f));
    CHECK_FALSE(sheet->GetRule(2).GetValue(StyleProperty::Filter).HasValue()); // not drawn yet
    // A plain number is a blur's sigma, as code sets it.
    CHECK(StyleValue::FloatVal(4.0f).AsFilter().Value().Blur == doctest::Approx(4.0f));

    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    EnsureGlobals();
    Init(ctx, root.Get(), 400, 300);
    ctx.SetStyleSheet(LoadSSS(u8".off { filter: grayscale(1) brightness(0.8); }"));
    auto leaf = RV(40, 30, 1);
    leaf->AddClass(u8"off"); // a leaf too: the filter needs the layer
    auto frame = New<FrameLayout>();
    frame->AddView(leaf.Get());
    root->AddView(frame.Get());
    LayoutPass(ctx, root.Get());
    vg::VGContext vgContext;
    vgContext.SetLayers(true);
    ctx.DrawRootView(root.Get(), vgContext);
    const vg::VGBatch& batch = vgContext.GetBatch();
    REQUIRE(batch.layers.Size() == 1u);
    CHECK(batch.layers[0].saturation == doctest::Approx(0.0f));
    CHECK(batch.layers[0].brightness == doctest::Approx(0.8f));
    CHECK(batch.layers[0].blur == doctest::Approx(0.0f));
}

TEST_CASE("box-model: a backdrop blur keeps to the corners the view's background rounds")
{
    // The backdrop's shape: its composite's geometry, a rounded outline where the background
    // rounds (and no corner-radius is set), four corners where nothing does.
    const auto backdropIndices = [](StringView sheetText) -> u32
    {
        UIContext ctx{DefaultAllocator()};
        auto root = MakeRoot();
        EnsureGlobals();
        Init(ctx, root.Get(), 400, 300);
        ctx.SetStyleSheet(LoadSSS(sheetText));
        auto under = RV(200, 200, 1);
        auto glass = RV(80, 60, 2);
        glass->AddClass(u8"glass");
        auto frame = New<FrameLayout>();
        frame->AddView(under.Get());
        frame->AddView(glass.Get());
        root->AddView(frame.Get());
        LayoutPass(ctx, root.Get());
        vg::VGContext vgContext;
        vgContext.SetLayers(true);
        ctx.DrawRootView(root.Get(), vgContext);
        const vg::VGBatch& batch = vgContext.GetBatch();
        for (usize i = 0; i < batch.CommandCount(); ++i)
        {
            const vg::VGCommand c = batch.GetCommand(i);
            if (c.drawMode == vg::VGDrawMode::Layer && c.backdrop)
            {
                return static_cast<u32>(c.indexCount);
            }
        }
        return 0;
    };
    const u32 square = backdropIndices(u8".glass { backdrop-filter: blur(6); background: #FFFFFF40; }");
    const u32 rounded =
        backdropIndices(u8".glass { backdrop-filter: blur(6); background: rounded-rect(#FFFFFF40, radius=10); }");
    REQUIRE(square > 0u);
    CHECK(rounded > square);
}

TEST_CASE("box-model: a drawable's var() colours resolve against the view drawing it")
{
    // One rule, several accents: the background takes --accent from the view it fills, and the
    // var()'s fallback where the view has none.
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRoot();
    EnsureGlobals();
    Init(ctx, root.Get(), 400, 300);
    ctx.SetStyleSheet(LoadSSS(u8".card { background: rounded-rect(var(--accent, #0000FF), radius=4, "
                              u8"border=var(--edge), border-width=1); }"
                              u8".red { --accent: #FF0000; }"
                              u8".green { --accent: #00FF00; }"
                              u8".wash { background: linear-gradient(90deg, var(--from), #000000); --from: #FFFF00; }"));
    const auto colourOf = [&](StringView classes) -> Color
    {
        auto frame = New<FrameLayout>();
        auto card = New<Panel>();
        LayoutStyle size;
        size.Width = SizeSpec::Fixed(Unit::Dp(40));
        size.Height = SizeSpec::Fixed(Unit::Dp(20));
        card->SetLayout(size);
        card->AddClass(u8"card");
        if (!classes.IsEmpty())
            card->AddClass(classes);
        frame->AddView(card.Get());
        root->AddView(frame.Get());
        LayoutPass(ctx, root.Get());
        vg::VGContext vgContext;
        ctx.DrawRootView(root.Get(), vgContext);
        root->RemoveView(frame.Get());
        const vg::VGBatch& batch = vgContext.GetBatch();
        return batch.vertices.IsEmpty() ? Color{-1, -1, -1, -1} : batch.vertices[0].color;
    };
    const Color red = colourOf(u8"red");
    CHECK(red.r > 0.99f);
    CHECK(red.b < 0.01f);
    const Color green = colourOf(u8"green");
    CHECK(green.g > 0.99f);
    CHECK(green.r < 0.01f);
    const Color fallback = colourOf(u8""); // no --accent: the var()'s fallback
    CHECK(fallback.b > 0.99f);
    CHECK(fallback.r < 0.01f);

    // The parse keeps the names beside the fallbacks.
    core::RefPtr<StyleSheet> sheet = LoadSSS(u8".x { background: linear-gradient(var(--a), #000000 50%, var(--b, #FFFFFF)); }"
                                             u8".y { background: color(var(--c)); }");
    REQUIRE(sheet);
    auto* gradient = core::Cast<GradientDrawable>(sheet->GetRule(0).GetValue(StyleProperty::Background).Value().AsDrawable());
    REQUIRE(gradient != nullptr);
    REQUIRE(gradient->StopVariables.Size() == 3u);
    CHECK(gradient->StopVariables[0] == StringView(u8"--a"));
    CHECK(gradient->StopVariables[1].IsEmpty());
    CHECK(gradient->Stops[2].color.r == doctest::Approx(1.0f)); // b's fallback
    auto* solid = core::Cast<ColorDrawable>(sheet->GetRule(1).GetValue(StyleProperty::Background).Value().AsDrawable());
    REQUIRE(solid != nullptr);
    CHECK(solid->ColorVariable == StringView(u8"--c"));
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
