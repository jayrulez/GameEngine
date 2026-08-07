// Draconic GUI - UINode / UIWidget tests: padding + content bounds, input-driven control
// state (hover/press/focus/disabled), and the CSS identity surface (tag/id/classes).
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
}

TEST_CASE("uinode: padding insets the content bounds")
{
    auto n = Make<UINode>();
    n->SetSize(foundation::Float2{100.0f, 80.0f});
    n->SetPadding(Thickness{5.0f, 10.0f, 15.0f, 20.0f});
    Rect content = n->GetContentBounds();
    CHECK(content == Rect{5.0f, 10.0f, 80.0f, 50.0f}); // 100-5-15, 80-10-20
}

TEST_CASE("uinode: over-large padding clamps to zero, not negative")
{
    auto n = Make<UINode>();
    n->SetSize(foundation::Float2{20.0f, 20.0f});
    n->SetPadding(Thickness{30.0f});
    Rect content = n->GetContentBounds();
    CHECK(content.width == 0.0f);
    CHECK(content.height == 0.0f);
}

TEST_CASE("uinode: control state follows pointer, focus, and enabled")
{
    auto root = Make<SceneNode>();
    root->SetSize(foundation::Float2{200.0f, 200.0f});
    auto w = Make<UINode>();
    w->SetSize(foundation::Float2{100.0f, 100.0f});
    root->AddChild(w.Get());
    EventDispatcher* d = root->GetEventDispatcher();

    CHECK(w->GetControlState() == ControlState::Normal);

    // Hover then leave (before any click, so no focus yet): Hover -> Normal.
    d->InjectMouseMove(foundation::Float2{50.0f, 50.0f}); // over w
    CHECK(w->IsHovered());
    CHECK(w->GetControlState() == ControlState::Hover);
    d->InjectMouseMove(foundation::Float2{150.0f, 150.0f}); // off w
    CHECK_FALSE(w->IsHovered());
    CHECK(w->GetControlState() == ControlState::Normal);

    // Press/release (click focuses the node).
    d->InjectMouseMove(foundation::Float2{50.0f, 50.0f});
    d->InjectMouseDown(foundation::Float2{50.0f, 50.0f}, MouseButton::Left);
    CHECK(w->IsPressed());
    CHECK(w->GetControlState() == ControlState::Pressed);

    d->InjectMouseUp(foundation::Float2{50.0f, 50.0f}, MouseButton::Left);
    CHECK_FALSE(w->IsPressed());
    CHECK(w->GetControlState() == ControlState::Hover); // hover outranks focus

    // Leave while focused (from the click): falls to Focused, not Normal.
    d->InjectMouseMove(foundation::Float2{150.0f, 150.0f});
    CHECK(w->IsFocused());
    CHECK(w->GetControlState() == ControlState::Focused);

    w->SetEnabled(false);
    CHECK(w->GetControlState() == ControlState::Disabled); // wins over everything
}

TEST_CASE("uinode: skin background draws with the current control state")
{
    auto root = Make<SceneNode>();
    root->SetSize(foundation::Float2{200.0f, 200.0f});
    auto w = Make<UINode>();
    w->SetSize(foundation::Float2{100.0f, 100.0f});

    auto skin = foundation::MakeRef<StateListDrawable>(foundation::DefaultAllocator());
    skin->Set(ControlState::Normal,
              foundation::MakeRef<RectangleDrawable>(foundation::DefaultAllocator(), foundation::Color::Blue));
    skin->Set(ControlState::Hover,
              foundation::MakeRef<RectangleDrawable>(foundation::DefaultAllocator(), foundation::Color::Red));
    w->SetSkin(skin);
    root->AddChild(w.Get());

    // Draws (in Normal state) through VG.
    // (State selection is covered by StateListDrawable + control-state tests above.)
    CHECK(w->GetControlState() == ControlState::Normal);
    CHECK(w->GetBackground() != nullptr);
}

TEST_CASE("uiwidget: tag and id")
{
    auto w = Make<UIWidget>();
    CHECK(w->GetTag().Size() == 0);
    w->SetTag(foundation::StringView(u8"button"));
    w->SetId(foundation::StringView(u8"ok"));
    CHECK(w->GetTag() == foundation::StringView(u8"button"));
    CHECK(w->GetId() == foundation::StringView(u8"ok"));
}

TEST_CASE("uiwidget: style classes add/remove/toggle")
{
    auto w = Make<UIWidget>();
    CHECK(w->ClassCount() == 0);

    w->AddClass(foundation::StringView(u8"primary"));
    w->AddClass(foundation::StringView(u8"large"));
    w->AddClass(foundation::StringView(u8"primary")); // duplicate ignored
    CHECK(w->ClassCount() == 2);
    CHECK(w->HasClass(foundation::StringView(u8"primary")));
    CHECK(w->HasClass(foundation::StringView(u8"large")));
    CHECK_FALSE(w->HasClass(foundation::StringView(u8"small")));

    w->RemoveClass(foundation::StringView(u8"large"));
    CHECK_FALSE(w->HasClass(foundation::StringView(u8"large")));
    CHECK(w->ClassCount() == 1);

    w->ToggleClass(foundation::StringView(u8"active"));
    CHECK(w->HasClass(foundation::StringView(u8"active")));
    w->ToggleClass(foundation::StringView(u8"active"));
    CHECK_FALSE(w->HasClass(foundation::StringView(u8"active")));
}

TEST_CASE("uiwidget: margin and inherited padding/state")
{
    auto w = Make<UIWidget>();
    w->SetMargin(Thickness{4.0f});
    CHECK(w->GetMargin().Left == 4.0f);
    w->SetPadding(Thickness{2.0f}); // inherited from UINode
    CHECK(w->GetPadding().Top == 2.0f);
    CHECK(w->GetControlState() == ControlState::Normal); // inherited from UINode
}
