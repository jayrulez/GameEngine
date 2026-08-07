// Draconic GUI - CSS transition tests: parse the `transition` shorthand, and animate a
// transitioned opacity change through the ActionManager (CSS -> Action-system integration).
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

    ResolvedStyle Style(const char8_t* name, const char8_t* value)
    {
        ResolvedStyle s;
        s.Set(SV(name), SV(value));
        return s;
    }
}

TEST_CASE("transition: parse shorthand")
{
    foundation::Array<TransitionDefinition> ts = ParseTransitions(SV(u8"opacity 0.3s"));
    REQUIRE(ts.Size() == 1);
    CHECK(ts[0].Property == SV(u8"opacity"));
    CHECK(ts[0].Duration == doctest::Approx(0.3f));

    foundation::Array<TransitionDefinition> multi =
        ParseTransitions(SV(u8"opacity 0.3s ease 0.1s, color 1s"));
    REQUIRE(multi.Size() == 2);
    CHECK(multi[0].Property == SV(u8"opacity"));
    CHECK(multi[0].Delay == doctest::Approx(0.1f));
    CHECK(multi[1].Property == SV(u8"color"));
    CHECK(multi[1].Duration == doctest::Approx(1.0f));
}

TEST_CASE("transition: opacity change animates through the ActionManager")
{
    auto root = Make<SceneNode>();
    auto node = Make<UINode>();
    root->AddChild(node.Get());

    ResolvedStyle oldStyle = Style(u8"opacity", u8"1");
    ResolvedStyle newStyle;
    newStyle.Set(SV(u8"opacity"), SV(u8"0"));
    newStyle.Set(SV(u8"transition"), SV(u8"opacity 0.5s"));

    ApplyStyleAnimated(*node.Get(), oldStyle, newStyle);
    CHECK(root->GetActionManager()->Count() == 1);    // a FadeAction was spawned
    CHECK(node->GetAlpha() == doctest::Approx(1.0f)); // rewound to the old value on start

    root->Update(Sec(0.25));
    CHECK(node->GetAlpha() == doctest::Approx(0.5f)); // halfway

    root->Update(Sec(0.25));
    CHECK(node->GetAlpha() == doctest::Approx(0.0f)); // arrived
    CHECK(root->GetActionManager()->IsEmpty());       // action finished
}

TEST_CASE("transition: without a transition declaration, opacity snaps")
{
    auto root = Make<SceneNode>();
    auto node = Make<UINode>();
    root->AddChild(node.Get());

    ResolvedStyle oldStyle = Style(u8"opacity", u8"1");
    ResolvedStyle newStyle = Style(u8"opacity", u8"0"); // no transition

    ApplyStyleAnimated(*node.Get(), oldStyle, newStyle);
    CHECK(node->GetAlpha() == doctest::Approx(0.0f)); // immediate
    CHECK(root->GetActionManager()->IsEmpty());       // nothing spawned
}

TEST_CASE("transition: unchanged value spawns no animation")
{
    auto root = Make<SceneNode>();
    auto node = Make<UINode>();
    root->AddChild(node.Get());

    ResolvedStyle oldStyle = Style(u8"opacity", u8"1");
    ResolvedStyle newStyle;
    newStyle.Set(SV(u8"opacity"), SV(u8"1")); // same value
    newStyle.Set(SV(u8"transition"), SV(u8"opacity 0.5s"));

    ApplyStyleAnimated(*node.Get(), oldStyle, newStyle);
    CHECK(root->GetActionManager()->IsEmpty()); // no change -> no action
}
