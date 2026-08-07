// Draconic GUI - Label / Button control tests: text properties, click handling through the
// dispatcher, control-state reaction, and CSS targeting the default `button` tag.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
import draconic.foundation;
import draconic.vg;
import draconic.gui;

using namespace draconic::gui;
namespace foundation = draconic::foundation;
namespace vg = draconic::vg;

namespace
{
    template <typename T>
    foundation::RefPtr<T> Make()
    {
        return foundation::MakeRef<T>(foundation::DefaultAllocator());
    }
    foundation::StringView SV(const char8_t* s) { return foundation::StringView(s); }
}

TEST_CASE("label: text properties")
{
    auto label = Make<Label>();
    label->SetText(SV(u8"Hello"));
    CHECK(label->GetText() == SV(u8"Hello"));
    label->SetTextColor(foundation::Color::Red);
    CHECK(label->GetTextColor().r == doctest::Approx(1.0f));
    CHECK(label->MeasureText().x == 0.0f); // no font yet -> zero measure
}

TEST_CASE("label: draws its background (text is a no-op without a font)")
{
    auto label = Make<Label>();
    label->SetSize(foundation::Float2{100.0f, 40.0f});
    label->SetText(SV(u8"Hi"));
    label->SetBackground(
        foundation::MakeRef<RectangleDrawable>(foundation::DefaultAllocator(), foundation::Color::Blue));

    vg::VGContext ctx;
    DrawContext dc{ctx};
    label->Draw(dc);
    CHECK(ctx.GetBatch().vertices.Size() > 0); // background geometry
}

TEST_CASE("button: defaults - tag and centered text")
{
    auto btn = Make<Button>();
    CHECK(btn->GetTag() == SV(u8"button")); // CSS `button` targets it
    btn->SetText(SV(u8"OK"));
    CHECK(btn->GetText() == SV(u8"OK"));
    CHECK_FALSE(btn->HasOnClick());
}

TEST_CASE("button: click callback fires on press+release")
{
    auto root = Make<SceneNode>();
    root->SetSize(foundation::Float2{200.0f, 200.0f});
    auto btn = Make<Button>();
    btn->SetSize(foundation::Float2{100.0f, 100.0f});
    root->AddChild(btn.Get());
    EventDispatcher* d = root->GetEventDispatcher();

    int clicks = 0;
    btn->SetOnClick([&clicks]() { ++clicks; });
    CHECK(btn->HasOnClick());

    d->InjectMouseDown(foundation::Float2{50.0f, 50.0f}, MouseButton::Left);
    d->InjectMouseUp(foundation::Float2{50.0f, 50.0f}, MouseButton::Left);
    CHECK(clicks == 1);

    // Press on the button, release elsewhere -> not a click.
    d->InjectMouseDown(foundation::Float2{50.0f, 50.0f}, MouseButton::Left);
    d->InjectMouseUp(foundation::Float2{300.0f, 300.0f}, MouseButton::Left);
    CHECK(clicks == 1);
}

TEST_CASE("button: control state reacts to the pointer")
{
    auto root = Make<SceneNode>();
    root->SetSize(foundation::Float2{200.0f, 200.0f});
    auto btn = Make<Button>();
    btn->SetSize(foundation::Float2{100.0f, 100.0f});
    root->AddChild(btn.Get());
    EventDispatcher* d = root->GetEventDispatcher();

    CHECK(btn->GetControlState() == ControlState::Normal);
    d->InjectMouseMove(foundation::Float2{50.0f, 50.0f});
    CHECK(btn->GetControlState() == ControlState::Hover);
    d->InjectMouseDown(foundation::Float2{50.0f, 50.0f}, MouseButton::Left);
    CHECK(btn->GetControlState() == ControlState::Pressed);
}

TEST_CASE("button: styled by CSS through the default tag")
{
    auto root = Make<SceneNode>();
    root->SetSize(foundation::Float2{200.0f, 200.0f});
    auto btn = Make<Button>();
    btn->SetSize(foundation::Float2{100.0f, 100.0f});
    root->AddChild(btn.Get());
    EventDispatcher* d = root->GetEventDispatcher();

    StyleManager mgr(
        CSSParser::Parse(SV(u8"button { opacity: 1; } button:hover { opacity: 0.3; }")));
    mgr.ApplyTree(*root.Get());
    CHECK(btn->GetAlpha() == doctest::Approx(1.0f));

    d->InjectMouseMove(foundation::Float2{50.0f, 50.0f}); // hover
    mgr.ApplyTree(*root.Get());                     // re-resolve -> :hover applies
    CHECK(btn->GetAlpha() == doctest::Approx(0.3f));
}
