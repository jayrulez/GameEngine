// Draconic GUI - TabWidget tests: adding tabs, first-tab auto-select, switching panels
// (visibility), the tab-changed callback, and click-to-switch through the dispatcher.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
import draconic.foundation;
import draconic.gui;

using namespace draconic::gui;
namespace foundation = draconic::foundation;

namespace
{
    template <typename T>
    foundation::RefPtr<T> Make()
    {
        return foundation::MakeRef<T>(foundation::DefaultAllocator());
    }
    foundation::RefPtr<Node> Panel() { return foundation::MakeRef<UIWidget>(foundation::DefaultAllocator()); }
}

TEST_CASE("tabwidget: first tab is auto-selected; panels switch visibility")
{
    auto tw = Make<TabWidget>();
    tw->SetSize(foundation::Float2{300.0f, 200.0f});

    auto p0 = Panel();
    auto p1 = Panel();
    auto p2 = Panel();
    tw->AddTab(foundation::StringView(u8"One"), p0.Get());
    tw->AddTab(foundation::StringView(u8"Two"), p1.Get());
    tw->AddTab(foundation::StringView(u8"Three"), p2.Get());

    CHECK(tw->TabCount() == 3);
    CHECK(tw->GetSelectedIndex() == 0);
    CHECK(p0->IsVisible());
    CHECK_FALSE(p1->IsVisible());
    CHECK_FALSE(p2->IsVisible());

    tw->SelectTab(2);
    CHECK(tw->GetSelectedIndex() == 2);
    CHECK_FALSE(p0->IsVisible());
    CHECK(p2->IsVisible());
}

TEST_CASE("tabwidget: tab-changed callback fires with the index")
{
    auto tw = Make<TabWidget>();
    tw->SetSize(foundation::Float2{300.0f, 200.0f});

    int changes = 0, last = -1;
    tw->SetOnTabChanged(
        [&](int i)
        {
            ++changes;
            last = i;
        });

    tw->AddTab(foundation::StringView(u8"A"), Panel().Get()); // auto-select 0 -> callback
    CHECK(changes == 1);
    CHECK(last == 0);
    tw->AddTab(foundation::StringView(u8"B"), Panel().Get()); // not auto-selected (not first)
    CHECK(changes == 1);
    tw->SelectTab(1);
    CHECK(changes == 2);
    CHECK(last == 1);

    tw->SelectTab(5); // out of range -> ignored
    CHECK(tw->GetSelectedIndex() == 1);
}

TEST_CASE("tabwidget: content panels are sized to the content host")
{
    auto tw = Make<TabWidget>();
    tw->SetSize(foundation::Float2{300.0f, 200.0f});
    tw->SetTabBarHeight(30.0f);

    auto p0 = Panel();
    tw->AddTab(foundation::StringView(u8"One"), p0.Get());
    // Host = full width, height minus the tab bar.
    CHECK(p0->GetSize().x == doctest::Approx(300.0f));
    CHECK(p0->GetSize().y == doctest::Approx(170.0f));
}

TEST_CASE("tabwidget: clicking a tab button switches the panel")
{
    auto root = Make<SceneNode>();
    root->SetSize(foundation::Float2{400.0f, 300.0f});
    auto tw = Make<TabWidget>();
    tw->SetSize(foundation::Float2{300.0f, 200.0f});
    tw->SetTabWidth(100.0f);
    tw->SetTabBarHeight(30.0f);
    root->AddChild(tw.Get());

    auto p0 = Panel();
    auto p1 = Panel();
    tw->AddTab(foundation::StringView(u8"One"), p0.Get());
    tw->AddTab(foundation::StringView(u8"Two"), p1.Get());
    EventDispatcher* d = root->GetEventDispatcher();

    // Tab bar spans y in [0,30); the second tab button is at x in [102, 202) (100 wide + 2 gap).
    d->InjectMouseDown(foundation::Float2{150.0f, 15.0f}, MouseButton::Left);
    d->InjectMouseUp(foundation::Float2{150.0f, 15.0f}, MouseButton::Left);
    CHECK(tw->GetSelectedIndex() == 1);
    CHECK(p1->IsVisible());
    CHECK_FALSE(p0->IsVisible());
}
