// Ported from Sedulous.UI.Tests/src/ScrollViewTests.bf (faithful; includes its MomentumHelper +
// ScrollBar cases). Beef get/set properties -> methods (scroll->SetScrollY / ScrollY()); the (dx,dy)
// tuple -> Float2. Logic only - no font service needed.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
import draconic.foundation;
import draconic.ui;
#include "TestHelpers.h"

using namespace draconic::ui;
using namespace draconic::ui::tests;
using namespace draconic::foundation;
namespace foundation = draconic::foundation;

static foundation::RefPtr<RootView> MakeRoot()
{
    return foundation::MakeRef<RootView>(foundation::DefaultAllocator());
}
static foundation::RefPtr<ScrollView> MakeScroll()
{
    return foundation::MakeRef<ScrollView>(foundation::DefaultAllocator());
}
static foundation::RefPtr<TestView> MakeContent(f32 w, f32 h)
{
    return foundation::MakeRef<TestView>(foundation::DefaultAllocator(), w, h);
}

// === ScrollView ===

TEST_CASE("scroll-view: ContentLargerThanViewport")
{
    UIContext ctx;
    auto root = MakeRoot();
    Init(ctx, root.Get(), 200, 100);
    auto scroll = MakeScroll();
    auto content = MakeContent(200, 500); // taller than viewport
    scroll->AddView(content.Get());
    root->AddView(scroll.Get());
    LayoutPass(ctx, root.Get());

    CHECK(scroll->MaxScrollY() > 0);
    CHECK(scroll->MaxScrollX() == 0);
}

TEST_CASE("scroll-view: ScrollClamps")
{
    UIContext ctx;
    auto root = MakeRoot();
    Init(ctx, root.Get(), 200, 100);
    auto scroll = MakeScroll();
    auto content = MakeContent(200, 500);
    scroll->AddView(content.Get());
    root->AddView(scroll.Get());
    LayoutPass(ctx, root.Get());

    scroll->SetScrollY(-100);
    CHECK(scroll->ScrollY() == 0);

    scroll->SetScrollY(9999);
    CHECK(scroll->ScrollY() == scroll->MaxScrollY());
}

TEST_CASE("scroll-view: ScrollTo")
{
    UIContext ctx;
    auto root = MakeRoot();
    Init(ctx, root.Get(), 200, 100);
    auto scroll = MakeScroll();
    auto content = MakeContent(200, 500);
    scroll->AddView(content.Get());
    root->AddView(scroll.Get());
    LayoutPass(ctx, root.Get());

    scroll->ScrollTo(0, 100);
    CHECK(scroll->ScrollY() == 100);
}

TEST_CASE("scroll-view: ScrollToTop")
{
    UIContext ctx;
    auto root = MakeRoot();
    Init(ctx, root.Get(), 200, 100);
    auto scroll = MakeScroll();
    auto content = MakeContent(200, 500);
    scroll->AddView(content.Get());
    root->AddView(scroll.Get());
    LayoutPass(ctx, root.Get());

    scroll->SetScrollY(200);
    scroll->ScrollToTop();
    CHECK(scroll->ScrollY() == 0);
}

TEST_CASE("scroll-view: ScrollToBottom")
{
    UIContext ctx;
    auto root = MakeRoot();
    Init(ctx, root.Get(), 200, 100);
    auto scroll = MakeScroll();
    auto content = MakeContent(200, 500);
    scroll->AddView(content.Get());
    root->AddView(scroll.Get());
    LayoutPass(ctx, root.Get());

    scroll->ScrollToBottom();
    CHECK(scroll->ScrollY() == scroll->MaxScrollY());
}

TEST_CASE("scroll-view: NeverPolicy_NoBar")
{
    UIContext ctx;
    auto root = MakeRoot();
    Init(ctx, root.Get(), 200, 100);
    auto scroll = MakeScroll();
    scroll->VScrollBarPolicy.SetValue(ScrollBarPolicy::Never);
    auto content = MakeContent(200, 500);
    scroll->AddView(content.Get());
    root->AddView(scroll.Get());
    LayoutPass(ctx, root.Get());

    // ViewportHeight should be full height (no bar reserved).
    CHECK(Abs(scroll->ViewportHeight() - 100) < 1.0f);
}

// === MomentumHelper ===

TEST_CASE("scroll-view: Momentum_Decelerates")
{
    MomentumHelper m;
    m.VelocityY = 500;
    CHECK(m.IsActive());

    f32 totalDy = 0.0f;
    for (int i = 0; i < 100; i++)
    {
        totalDy += m.Update(0.016f).y;
    }

    CHECK(totalDy > 0);
    CHECK((!m.IsActive() || Abs(m.VelocityY) < 1.0f));
}

TEST_CASE("scroll-view: Momentum_Stop")
{
    MomentumHelper m;
    m.VelocityY = 500;
    m.Stop();
    CHECK(!m.IsActive());
}

// === ScrollBar ===

TEST_CASE("scroll-view: ScrollBar_ValueClamps")
{
    auto bar = foundation::MakeRef<ScrollBar>(foundation::DefaultAllocator());
    bar->SetMaxValue(100);
    bar->SetValue(-10);
    CHECK(bar->Value() == 0);
    bar->SetValue(200);
    CHECK(bar->Value() == 100);
}

TEST_CASE("scroll-view: ScrollBar_ValueChanged")
{
    auto bar = foundation::MakeRef<ScrollBar>(foundation::DefaultAllocator());
    bar->SetMaxValue(100);
    f32 lastVal = -1;
    bar->OnValueChanged.Add(
        Event<void(ScrollBar*, f32)>::Handler{[&lastVal](ScrollBar*, f32 v) { lastVal = v; }});
    bar->SetValue(42);
    CHECK(lastVal == 42);
}
