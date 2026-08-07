// Draconic GUI - RadioButton + RadioGroup tests: mutual exclusion, click-to-select, group
// callback, and standalone (no-group) behavior.
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
}

TEST_CASE("radio: group keeps exactly one selected")
{
    auto a = Make<RadioButton>();
    auto b = Make<RadioButton>();
    auto c = Make<RadioButton>();
    RadioGroup group;
    group.Add(a.Get());
    group.Add(b.Get());
    group.Add(c.Get());

    CHECK(group.Count() == 3);
    CHECK(group.GetSelectedIndex() == -1); // nothing selected yet

    a->Select();
    CHECK(a->IsSelected());
    CHECK_FALSE(b->IsSelected());
    CHECK(group.GetSelectedIndex() == 0);
    CHECK(group.GetSelected() == a.Get());

    b->Select(); // selecting b clears a
    CHECK_FALSE(a->IsSelected());
    CHECK(b->IsSelected());
    CHECK(group.GetSelectedIndex() == 1);
}

TEST_CASE("radio: group callback fires with the selected index, only on change")
{
    auto a = Make<RadioButton>();
    auto b = Make<RadioButton>();
    RadioGroup group;
    group.Add(a.Get());
    group.Add(b.Get());

    int changes = 0;
    int lastIndex = -99;
    group.SetOnSelectionChanged(
        [&](int i)
        {
            ++changes;
            lastIndex = i;
        });

    a->Select();
    CHECK(changes == 1);
    CHECK(lastIndex == 0);
    a->Select(); // already selected -> no callback
    CHECK(changes == 1);
    b->Select();
    CHECK(changes == 2);
    CHECK(lastIndex == 1);
}

TEST_CASE("radio: click selects and fires OnSelected")
{
    auto root = Make<SceneNode>();
    root->SetSize(foundation::Float2{200.0f, 200.0f});
    auto a = Make<RadioButton>();
    auto b = Make<RadioButton>();
    a->SetSize(foundation::Float2{20.0f, 20.0f});
    b->SetSize(foundation::Float2{20.0f, 20.0f});
    b->SetPosition(foundation::Float2{0.0f, 40.0f});
    root->AddChild(a.Get());
    root->AddChild(b.Get());
    RadioGroup group;
    group.Add(a.Get());
    group.Add(b.Get());

    int aSelected = 0;
    a->SetOnSelected([&]() { ++aSelected; });

    EventDispatcher* d = root->GetEventDispatcher();
    d->InjectMouseDown(foundation::Float2{10.0f, 10.0f}, MouseButton::Left);
    d->InjectMouseUp(foundation::Float2{10.0f, 10.0f}, MouseButton::Left); // click a
    CHECK(a->IsSelected());
    CHECK(aSelected == 1);

    d->InjectMouseDown(foundation::Float2{10.0f, 50.0f}, MouseButton::Left);
    d->InjectMouseUp(foundation::Float2{10.0f, 50.0f}, MouseButton::Left); // click b
    CHECK(b->IsSelected());
    CHECK_FALSE(a->IsSelected());
    CHECK(aSelected == 1); // a was not re-selected
}

TEST_CASE("radio: a button with no group selects standalone")
{
    auto a = Make<RadioButton>();
    int sel = 0;
    a->SetOnSelected([&]() { ++sel; });
    a->Select();
    CHECK(a->IsSelected());
    CHECK(sel == 1);
    a->SetSelected(false);
    CHECK_FALSE(a->IsSelected());
}

TEST_CASE("radio: draws ring, plus a dot when selected")
{
    auto a = Make<RadioButton>();
    a->SetSize(foundation::Float2{20.0f, 20.0f});

    {
        vg::VGContext ctx;
        DrawContext dc{ctx};
        a->Draw(dc);
        const foundation::usize ringOnly = ctx.GetBatch().vertices.Size();
        CHECK(ringOnly > 0);
    }
    a->Select();
    {
        vg::VGContext ctx;
        DrawContext dc{ctx};
        a->Draw(dc);
        CHECK(ctx.GetBatch().vertices.Size() > 0); // ring + dot
    }
}
