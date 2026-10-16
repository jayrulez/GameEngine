// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - directional navigation (G7 of Specs/vg-and-gui-lab.md): the geometric picker over grids
// and irregular layouts, focus scopes (pass, wrap, contain, focus memory, default focus), widgets
// that use the arrows and give them back at their ends, and activation by Return.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import experimental.gui;

using namespace experimental::gui;
namespace core = foundation::core;

namespace
{
    template <typename T>
    core::RefPtr<T> Make()
    {
        return core::MakeRef<T>(core::DefaultAllocator());
    }
    core::StringView SV(const char8_t* s) { return core::StringView(s); }

    // A focusable widget at (x, y), 40 x 20, id `id`.
    core::RefPtr<Button> Cell(Node& parent, const char8_t* id, float x, float y, float w = 40.0f, float h = 20.0f)
    {
        auto b = Make<Button>();
        b->SetId(SV(id));
        b->SetPosition(core::Float2{x, y});
        b->SetSize(core::Float2{w, h});
        parent.AddChild(b.Get());
        return b;
    }

    struct Bed
    {
        core::RefPtr<SceneRoot> root = Make<SceneRoot>();
        EventDispatcher& d = *root->GetEventDispatcher();
        Bed() { root->SetSize(core::Float2{400.0f, 400.0f}); }
        [[nodiscard]] core::StringView Focused() const
        {
            const UIWidget* w = core::Cast<UIWidget>(d.GetFocusNode());
            return w != nullptr ? w->GetId() : SV(u8"");
        }
        void Key(KeyCode key) { d.InjectKeyDown(static_cast<core::u32>(key)); }
    };
}

TEST_CASE("navigation: a grid moves by rows and columns, and stops at its edges")
{
    Bed bed;
    // a b c
    // d e f
    const char8_t* ids[6] = {u8"a", u8"b", u8"c", u8"d", u8"e", u8"f"};
    core::RefPtr<Button> cells[6];
    for (int i = 0; i < 6; ++i)
        cells[i] = Cell(*bed.root.Get(), ids[i], 10.0f + 60.0f * (i % 3), 10.0f + 40.0f * (i / 3));

    bed.Key(KeyCode::Down); // nothing focused: the first
    CHECK(bed.Focused() == SV(u8"a"));
    bed.Key(KeyCode::Right);
    CHECK(bed.Focused() == SV(u8"b"));
    bed.Key(KeyCode::Down);
    CHECK(bed.Focused() == SV(u8"e"));
    bed.Key(KeyCode::Right);
    CHECK(bed.Focused() == SV(u8"f"));
    bed.Key(KeyCode::Right); // the edge
    CHECK(bed.Focused() == SV(u8"f"));
    bed.Key(KeyCode::Up);
    CHECK(bed.Focused() == SV(u8"c"));
}

TEST_CASE("navigation: an overlapping neighbour beats a diagonally nearer one")
{
    Bed bed;
    auto from = Cell(*bed.root.Get(), u8"from", 100.0f, 100.0f);
    auto diagonal = Cell(*bed.root.Get(), u8"diagonal", 150.0f, 130.0f); // nearer, but off the row
    auto row = Cell(*bed.root.Get(), u8"row", 260.0f, 100.0f);           // farther, on the row
    bed.d.SetFocusNode(from.Get());
    bed.Key(KeyCode::Right);
    CHECK(bed.Focused() == SV(u8"row"));

    // Irregular: with nothing on the row, the nearest beyond wins.
    row->SetVisible(false);
    bed.d.SetFocusNode(from.Get());
    bed.Key(KeyCode::Right);
    CHECK(bed.Focused() == SV(u8"diagonal"));
}

TEST_CASE("navigation: hidden, disabled and clipped-away widgets are skipped")
{
    Bed bed;
    auto from = Cell(*bed.root.Get(), u8"from", 10.0f, 10.0f);
    auto hidden = Cell(*bed.root.Get(), u8"hidden", 10.0f, 40.0f);
    auto disabled = Cell(*bed.root.Get(), u8"disabled", 10.0f, 70.0f);
    auto box = Make<UIWidget>();
    box->SetPosition(core::Float2{0.0f, 100.0f});
    box->SetSize(core::Float2{60.0f, 30.0f});
    box->SetClipChildren(true);
    bed.root->AddChild(box.Get());
    auto clipped = Cell(*box.Get(), u8"clipped", 10.0f, 200.0f); // outside its parent's clip
    auto target = Cell(*bed.root.Get(), u8"target", 10.0f, 160.0f);
    hidden->SetVisible(false);
    disabled->SetEnabled(false);
    bed.d.SetFocusNode(from.Get());
    bed.Key(KeyCode::Down);
    CHECK(bed.Focused() == SV(u8"target"));
}

TEST_CASE("navigation: scopes pass, wrap and contain; coming back returns to the remembered widget")
{
    Bed bed;
    auto menu = Make<UIWidget>(); // a column on the left
    menu->SetSize(core::Float2{100.0f, 200.0f});
    bed.root->AddChild(menu.Get());
    menu->SetNavScope(NavScope::Wrap);
    auto one = Cell(*menu.Get(), u8"one", 10.0f, 10.0f);
    auto two = Cell(*menu.Get(), u8"two", 10.0f, 40.0f);
    auto three = Cell(*menu.Get(), u8"three", 10.0f, 70.0f);
    auto side = Cell(*bed.root.Get(), u8"side", 200.0f, 40.0f); // outside the menu, to its right

    bed.d.SetFocusNode(three.Get());
    bed.Key(KeyCode::Down); // wraps to the top
    CHECK(bed.Focused() == SV(u8"one"));
    bed.Key(KeyCode::Up); // and back round
    CHECK(bed.Focused() == SV(u8"three"));

    // A pass scope hands an edge on: Right finds nothing inside the menu, so the widget beside it.
    menu->SetNavScope(NavScope::Pass);
    bed.d.SetFocusNode(two.Get());
    bed.Key(KeyCode::Right);
    CHECK(bed.Focused() == SV(u8"side"));
    bed.Key(KeyCode::Left); // back into the menu: the remembered widget, not the nearest
    CHECK(bed.Focused() == SV(u8"two"));

    menu->SetNavScope(NavScope::Contain);
    bed.Key(KeyCode::Right); // a dialog's edge
    CHECK(bed.Focused() == SV(u8"two"));
}

TEST_CASE("navigation: entering a scope with no memory takes its default focus")
{
    Bed bed;
    auto start = Cell(*bed.root.Get(), u8"start", 10.0f, 10.0f);
    auto panel = Make<UIWidget>();
    panel->SetPosition(core::Float2{0.0f, 100.0f});
    panel->SetSize(core::Float2{300.0f, 100.0f});
    panel->SetNavScope(NavScope::Pass);
    panel->SetDefaultFocusId(SV(u8"ok"));
    bed.root->AddChild(panel.Get());
    auto cancel = Cell(*panel.Get(), u8"cancel", 10.0f, 10.0f); // nearest from above
    auto ok = Cell(*panel.Get(), u8"ok", 200.0f, 10.0f);
    bed.d.SetFocusNode(start.Get());
    bed.Key(KeyCode::Down);
    CHECK(bed.Focused() == SV(u8"ok"));
}

TEST_CASE("navigation: a modal confines it")
{
    Bed bed;
    auto behind = Cell(*bed.root.Get(), u8"behind", 10.0f, 10.0f);
    auto dialog = Make<UIWidget>();
    dialog->SetPosition(core::Float2{100.0f, 100.0f});
    dialog->SetSize(core::Float2{200.0f, 100.0f});
    bed.root->AddChild(dialog.Get());
    auto yes = Cell(*dialog.Get(), u8"yes", 10.0f, 10.0f);
    auto no = Cell(*dialog.Get(), u8"no", 100.0f, 10.0f);
    bed.d.SetModalRoot(dialog.Get());
    bed.Key(KeyCode::Right); // nothing focused: the modal's first
    CHECK(bed.Focused() == SV(u8"yes"));
    bed.Key(KeyCode::Right);
    CHECK(bed.Focused() == SV(u8"no"));
    bed.Key(KeyCode::Up); // the widget behind is out of reach
    CHECK(bed.Focused() == SV(u8"no"));
}

TEST_CASE("navigation: a slider and a list use the arrows, and give them back at their ends")
{
    Bed bed;
    auto slider = Make<Slider>();
    slider->SetPosition(core::Float2{10.0f, 10.0f});
    slider->SetSize(core::Float2{100.0f, 20.0f});
    bed.root->AddChild(slider.Get());
    auto after = Cell(*bed.root.Get(), u8"after", 200.0f, 10.0f);
    bed.d.SetFocusNode(slider.Get());
    slider->SetValue(0.9f);
    bed.Key(KeyCode::Right);
    CHECK(slider->GetValue() == doctest::Approx(0.95f));
    bed.Key(KeyCode::Right);
    bed.Key(KeyCode::Right); // at 1: given back
    CHECK(bed.Focused() == SV(u8"after"));

    auto list = Make<ListBox>();
    list->SetPosition(core::Float2{10.0f, 100.0f});
    list->SetSize(core::Float2{100.0f, 80.0f});
    list->AddItem(SV(u8"x"));
    list->AddItem(SV(u8"y"));
    bed.root->AddChild(list.Get());
    auto below = Cell(*bed.root.Get(), u8"below", 10.0f, 250.0f);
    bed.d.SetFocusNode(list.Get());
    list->SetSelectedIndex(0);
    bed.Key(KeyCode::Down);
    CHECK(list->GetSelectedIndex() == 1);
    CHECK(bed.d.GetFocusNode() == list.Get());
    bed.Key(KeyCode::Down); // at the end: given back
    CHECK(bed.Focused() == SV(u8"below"));
}

TEST_CASE("navigation: Return activates the focused button as a click does")
{
    Bed bed;
    auto go = Cell(*bed.root.Get(), u8"go", 10.0f, 10.0f);
    int clicks = 0;
    (void)go->AddEventListener(EventType::MouseClick, [&](const Event&) { ++clicks; });
    auto check = Make<CheckBox>();
    check->SetPosition(core::Float2{10.0f, 60.0f});
    check->SetSize(core::Float2{20.0f, 20.0f});
    bed.root->AddChild(check.Get());

    bed.d.SetFocusNode(go.Get());
    bed.Key(KeyCode::Return);
    CHECK(clicks == 1);
    bed.d.SetFocusNode(check.Get());
    bed.Key(KeyCode::Space);
    CHECK(check->IsChecked());
}
