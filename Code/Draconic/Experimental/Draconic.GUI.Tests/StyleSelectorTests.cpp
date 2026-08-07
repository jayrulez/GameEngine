// Draconic GUI - CSS StyleSelector tests: parsing, specificity, and matching against the
// UIWidget identity (tag/#id/.class), pseudo-classes via control state, and descendant /
// child combinators. Derived from eepp css selector semantics (common subset).
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
    StyleSelector Sel(const char8_t* s) { return StyleSelector(foundation::StringView(s)); }

    foundation::RefPtr<UIWidget> Widget(const char8_t* tag, const char8_t* id = u8"")
    {
        auto w = Make<UIWidget>();
        w->SetTag(foundation::StringView(tag));
        if (id[0] != 0)
            w->SetId(foundation::StringView(id));
        return w;
    }
}

TEST_CASE("selector: specificity buckets")
{
    CHECK(Sel(u8"button").Specificity() == 1);      // tag
    CHECK(Sel(u8".primary").Specificity() == 1024); // class
    CHECK(Sel(u8"#ok").Specificity() == 1048576);   // id
    CHECK(Sel(u8"button.primary#ok").Specificity() == 1048576 + 1024 + 1);
    CHECK(Sel(u8"*").Specificity() == 0);                   // universal
    CHECK(Sel(u8"div .item").Specificity() == 1 + 1024);    // tag + class across two rules
    CHECK(Sel(u8"button:hover").Specificity() == 1 + 1024); // pseudo counts as a class
}

TEST_CASE("selector: tag / id / class matching")
{
    auto w = Widget(u8"button", u8"ok");
    w->AddClass(foundation::StringView(u8"primary"));

    CHECK(Sel(u8"button").Select(*w.Get()));
    CHECK_FALSE(Sel(u8"span").Select(*w.Get()));
    CHECK(Sel(u8"#ok").Select(*w.Get()));
    CHECK_FALSE(Sel(u8"#cancel").Select(*w.Get()));
    CHECK(Sel(u8".primary").Select(*w.Get()));
    CHECK_FALSE(Sel(u8".large").Select(*w.Get()));
    CHECK(Sel(u8"button.primary#ok").Select(*w.Get()));
    CHECK_FALSE(Sel(u8"button.large").Select(*w.Get()));
    CHECK(Sel(u8"*").Select(*w.Get())); // universal matches
}

TEST_CASE("selector: descendant vs child combinators")
{
    auto container = Make<UIWidget>();
    container->AddClass(foundation::StringView(u8"container"));
    auto mid = Make<UIWidget>();
    auto deep = Widget(u8"button");
    container->AddChild(mid.Get());
    mid->AddChild(deep.Get()); // container > mid > deep(button)

    // Descendant: container is an ancestor of the button.
    CHECK(Sel(u8".container button").Select(*deep.Get()));
    // Child: button's direct parent is `mid`, not `.container` -> no match.
    CHECK_FALSE(Sel(u8".container > button").Select(*deep.Get()));

    auto direct = Widget(u8"button");
    container->AddChild(direct.Get()); // container > direct(button)
    CHECK(Sel(u8".container > button").Select(*direct.Get()));
    CHECK(Sel(u8".container button").Select(*direct.Get()));
}

TEST_CASE("selector: pseudo-classes track control state")
{
    auto root = Make<SceneNode>();
    root->SetSize(foundation::Float2{200.0f, 200.0f});
    auto w = Widget(u8"button");
    w->SetSize(foundation::Float2{100.0f, 100.0f});
    root->AddChild(w.Get());
    EventDispatcher* d = root->GetEventDispatcher();

    CHECK_FALSE(Sel(u8"button:hover").Select(*w.Get()));                  // not hovered yet
    CHECK(Sel(u8"button:hover").Select(*w.Get(), /*applyPseudo*/ false)); // pseudo ignored

    d->InjectMouseMove(foundation::Float2{50.0f, 50.0f}); // hover w
    CHECK(Sel(u8"button:hover").Select(*w.Get()));

    d->InjectMouseDown(foundation::Float2{50.0f, 50.0f}, MouseButton::Left); // press + focus
    CHECK(Sel(u8"button:active").Select(*w.Get()));
    CHECK(Sel(u8"button:focus").Select(*w.Get()));

    w->SetEnabled(false);
    CHECK(Sel(u8":disabled").Select(*w.Get()));
    CHECK(Sel(u8"button:disabled").Select(*w.Get()));
}
