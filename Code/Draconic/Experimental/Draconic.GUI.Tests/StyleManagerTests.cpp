// Draconic GUI - StyleManager tests: a StyleSheet applied live to a widget tree, including
// the end-to-end showcase where hovering re-resolves :hover rules and drives a CSS
// transition through the ActionManager.
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
    foundation::StringView SV(const char8_t* s) { return foundation::StringView(s); }
    foundation::Duration Sec(double s) { return foundation::Duration::FromSeconds(s); }

    foundation::RefPtr<UIWidget> Widget(const char8_t* tag, foundation::Float2 pos, foundation::Float2 size)
    {
        auto w = Make<UIWidget>();
        w->SetTag(SV(tag));
        w->SetPosition(pos);
        w->SetSize(size);
        return w;
    }
}

TEST_CASE("style-manager: applies a sheet across the tree")
{
    auto root = Make<SceneNode>();
    root->SetSize(foundation::Float2{200.0f, 200.0f});
    auto btn = Widget(u8"button", foundation::Float2{0.0f, 0.0f}, foundation::Float2{100.0f, 100.0f});
    root->AddChild(btn.Get());

    StyleManager mgr(CSSParser::Parse(SV(u8"button { opacity: 0.5; background-color: red; }")));
    mgr.ApplyTree(*root.Get());

    CHECK(btn->GetAlpha() == doctest::Approx(0.5f));
    CHECK(btn->GetBackground() != nullptr);
}

TEST_CASE("style-manager: hover re-resolve drives a live transition")
{
    auto root = Make<SceneNode>();
    root->SetSize(foundation::Float2{200.0f, 200.0f});
    auto btn = Widget(u8"button", foundation::Float2{0.0f, 0.0f}, foundation::Float2{100.0f, 100.0f});
    root->AddChild(btn.Get());
    EventDispatcher* d = root->GetEventDispatcher();

    // transition on the base rule (applies both directions); :hover changes opacity.
    StyleManager mgr(CSSParser::Parse(
        SV(u8"button { opacity: 1; transition: opacity 0.5s; } button:hover { opacity: 0.4; }")));

    mgr.ApplyTree(*root.Get()); // first apply (snap)
    CHECK(btn->GetAlpha() == doctest::Approx(1.0f));

    d->InjectMouseMove(foundation::Float2{50.0f, 50.0f}); // hover the button
    CHECK(btn->IsHovered());

    mgr.ApplyTree(*root.Get()); // re-resolve: :hover now matches -> transition animates
    CHECK(root->GetActionManager()->Count() == 1);
    CHECK(btn->GetAlpha() == doctest::Approx(1.0f)); // rewound to start

    root->Update(Sec(0.25));
    CHECK(btn->GetAlpha() == doctest::Approx(0.7f)); // 1 + (0.4-1)*0.5
    root->Update(Sec(0.25));
    CHECK(btn->GetAlpha() == doctest::Approx(0.4f)); // arrived at :hover value
}

TEST_CASE("style-manager: media context gates what applies")
{
    auto root = Make<SceneNode>();
    root->SetSize(foundation::Float2{200.0f, 200.0f});
    auto btn = Widget(u8"button", foundation::Float2{0.0f, 0.0f}, foundation::Float2{100.0f, 100.0f});
    root->AddChild(btn.Get());

    StyleManager mgr(CSSParser::Parse(
        SV(u8"button { opacity: 1; } @media (min-width: 600px) { button { opacity: 0.2; } }")));

    mgr.SetMediaContext(MediaContext{400.0f, 0.0f, 96.0f}); // narrow
    mgr.ApplyTree(*root.Get());
    CHECK(btn->GetAlpha() == doctest::Approx(1.0f));

    mgr.Clear(); // forget cached styles so the re-apply is a fresh snap
    mgr.SetMediaContext(MediaContext{800.0f, 0.0f, 96.0f}); // wide
    mgr.ApplyTree(*root.Get());
    CHECK(btn->GetAlpha() == doctest::Approx(0.2f));
}

TEST_CASE("style-manager: SetStyleSheet swaps styles (hot-reload)")
{
    auto root = Make<SceneNode>();
    root->SetSize(foundation::Float2{200.0f, 200.0f});
    auto btn = Widget(u8"button", foundation::Float2{0.0f, 0.0f}, foundation::Float2{100.0f, 100.0f});
    root->AddChild(btn.Get());

    StyleManager mgr(CSSParser::Parse(SV(u8"button { opacity: 1; }")));
    mgr.ApplyTree(*root.Get());
    CHECK(btn->GetAlpha() == doctest::Approx(1.0f));

    mgr.SetStyleSheet(CSSParser::Parse(SV(u8"button { opacity: 0.3; }")));
    mgr.ApplyTree(*root.Get());
    CHECK(btn->GetAlpha() == doctest::Approx(0.3f));
}
