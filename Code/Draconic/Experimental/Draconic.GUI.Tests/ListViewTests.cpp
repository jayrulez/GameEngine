// Draconic GUI - ListView tests: virtualization (only visible rows realized), single selection
// via mouse + keyboard, wheel/scroll-into-view, and reacting to model updates.
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

    foundation::Array<foundation::String> MakeItems(int count)
    {
        foundation::Array<foundation::String> items;
        for (int i = 0; i < count; ++i)
            items.PushBack(foundation::String(SV(u8"item")));
        return items;
    }
}

TEST_CASE("listview: virtualizes - only visible rows are realized")
{
    StringListModel model(MakeItems(1000));
    auto root = Make<SceneNode>();
    root->SetSize(foundation::Float2{300.0f, 300.0f});
    auto list = Make<ListView>();
    list->SetSize(foundation::Float2{200.0f, 100.0f});
    list->SetRowHeight(20.0f);
    root->AddChild(list.Get());
    list->SetModel(&model);

    // 100px viewport / 20px rows -> ~5 rows + 2 buffer = 7 realized, NOT 1000.
    CHECK(list->VisibleRowCount() == 7);
    CHECK(list->GetModel() == &model);
}

TEST_CASE("listview: clicking a row selects it and fires the callback")
{
    StringListModel model(MakeItems(10));
    auto root = Make<SceneNode>();
    root->SetSize(foundation::Float2{300.0f, 300.0f});
    auto list = Make<ListView>();
    list->SetSize(foundation::Float2{200.0f, 100.0f});
    list->SetRowHeight(20.0f);
    root->AddChild(list.Get());
    list->SetModel(&model);
    EventDispatcher* d = root->GetEventDispatcher();

    int selectedRow = -1;
    int calls = 0;
    list->SetOnSelectionChanged(
        [&](ModelIndex i)
        {
            selectedRow = i.Row;
            ++calls;
        });

    // Row 2 spans y in [40, 60); click it.
    d->InjectMouseDown(foundation::Float2{50.0f, 50.0f}, MouseButton::Left);
    d->InjectMouseUp(foundation::Float2{50.0f, 50.0f}, MouseButton::Left);
    CHECK(list->GetSelectedRow() == 2);
    CHECK(selectedRow == 2);
    CHECK(calls == 1);
    CHECK(list->GetSelectedIndex() == MakeModelIndex(2));
}

TEST_CASE("listview: keyboard navigation moves the selection")
{
    StringListModel model(MakeItems(10));
    auto root = Make<SceneNode>();
    root->SetSize(foundation::Float2{300.0f, 300.0f});
    auto list = Make<ListView>();
    list->SetSize(foundation::Float2{200.0f, 100.0f});
    list->SetRowHeight(20.0f);
    root->AddChild(list.Get());
    list->SetModel(&model);
    EventDispatcher* d = root->GetEventDispatcher();
    list->RequestFocus();

    const auto key = [](KeyCode k) { return static_cast<foundation::u32>(k); };
    d->InjectKeyDown(key(KeyCode::Down)); // nothing selected -> row 0
    CHECK(list->GetSelectedRow() == 0);
    d->InjectKeyDown(key(KeyCode::Down));
    d->InjectKeyDown(key(KeyCode::Down));
    CHECK(list->GetSelectedRow() == 2);
    d->InjectKeyDown(key(KeyCode::Up));
    CHECK(list->GetSelectedRow() == 1);
    d->InjectKeyDown(key(KeyCode::End));
    CHECK(list->GetSelectedRow() == 9);
    d->InjectKeyDown(key(KeyCode::Home));
    CHECK(list->GetSelectedRow() == 0);
}

TEST_CASE("listview: selecting a far row scrolls it into view")
{
    StringListModel model(MakeItems(20)); // content 400, viewport 100 -> maxScroll 300
    auto root = Make<SceneNode>();
    root->SetSize(foundation::Float2{300.0f, 300.0f});
    auto list = Make<ListView>();
    list->SetSize(foundation::Float2{200.0f, 100.0f});
    list->SetRowHeight(20.0f);
    root->AddChild(list.Get());
    list->SetModel(&model);

    CHECK(list->ScrollOffset() == doctest::Approx(0.0f));
    list->SetSelectedRow(19); // last row -> bottom (400) - viewport (100) = 300
    CHECK(list->ScrollOffset() == doctest::Approx(300.0f));
    CHECK(list->GetSelectedRow() == 19);
}

TEST_CASE("listview: wheel scrolls and clamps")
{
    StringListModel model(MakeItems(20));
    auto root = Make<SceneNode>();
    root->SetSize(foundation::Float2{300.0f, 300.0f});
    auto list = Make<ListView>();
    list->SetSize(foundation::Float2{200.0f, 100.0f});
    list->SetRowHeight(20.0f);
    root->AddChild(list.Get());
    list->SetModel(&model);
    EventDispatcher* d = root->GetEventDispatcher();

    d->InjectMouseWheel(foundation::Float2{50.0f, 50.0f}, foundation::Float2{0.0f, -3.0f}); // down 3 rows
    CHECK(list->ScrollOffset() == doctest::Approx(60.0f));
    d->InjectMouseWheel(foundation::Float2{50.0f, 50.0f},
                        foundation::Float2{0.0f, -100.0f}); // clamps to maxScroll
    CHECK(list->ScrollOffset() == doctest::Approx(300.0f));
}

TEST_CASE("listview: reacts to model updates and clamps a stale selection")
{
    StringListModel model(MakeItems(5));
    auto root = Make<SceneNode>();
    root->SetSize(foundation::Float2{300.0f, 300.0f});
    auto list = Make<ListView>();
    list->SetSize(foundation::Float2{200.0f, 100.0f});
    list->SetRowHeight(20.0f);
    root->AddChild(list.Get());
    list->SetModel(&model);

    list->SetSelectedRow(4);
    CHECK(list->GetSelectedRow() == 4);

    model.SetItems(MakeItems(2)); // notifies the view -> selection 4 is now out of range
    CHECK(list->GetSelectedRow() == -1);

    model.AddItem(SV(u8"more"));
    CHECK(list->GetModel()->RowCount() == 3);
}

// ===================== Multi-selection (AbstractItemView) =====================

TEST_CASE("itemview: Ctrl-click toggles items in multi-selection mode")
{
    StringListModel model(MakeItems(10));
    auto root = Make<SceneNode>();
    root->SetSize(foundation::Float2{300.0f, 300.0f});
    auto list = Make<ListView>();
    list->SetSize(foundation::Float2{200.0f, 120.0f});
    list->SetRowHeight(20.0f);
    list->SetSelectionMode(SelectionMode::Multi);
    root->AddChild(list.Get());
    list->SetModel(&model);
    EventDispatcher* d = root->GetEventDispatcher();
    const foundation::u32 ctrl = static_cast<foundation::u32>(KeyModCtrl);

    d->InjectMouseDown(foundation::Float2{10.0f, 10.0f}, MouseButton::Left); // row 0 (plain)
    d->InjectMouseUp(foundation::Float2{10.0f, 10.0f}, MouseButton::Left);
    d->InjectMouseDown(foundation::Float2{10.0f, 50.0f}, MouseButton::Left, ctrl); // row 2 (ctrl)
    d->InjectMouseUp(foundation::Float2{10.0f, 50.0f}, MouseButton::Left, ctrl);

    CHECK(list->SelectedItems().Size() == 2);
    CHECK(list->IsItemSelected(0));
    CHECK(list->IsItemSelected(2));

    // Ctrl-click an already-selected item removes it.
    d->InjectMouseDown(foundation::Float2{10.0f, 10.0f}, MouseButton::Left, ctrl);
    d->InjectMouseUp(foundation::Float2{10.0f, 10.0f}, MouseButton::Left, ctrl);
    CHECK_FALSE(list->IsItemSelected(0));
    CHECK(list->SelectedItems().Size() == 1);
}

TEST_CASE("itemview: Shift-click selects a range from the anchor")
{
    StringListModel model(MakeItems(10));
    auto root = Make<SceneNode>();
    root->SetSize(foundation::Float2{300.0f, 300.0f});
    auto list = Make<ListView>();
    list->SetSize(foundation::Float2{200.0f, 120.0f});
    list->SetRowHeight(20.0f);
    list->SetSelectionMode(SelectionMode::Multi);
    root->AddChild(list.Get());
    list->SetModel(&model);
    EventDispatcher* d = root->GetEventDispatcher();

    d->InjectMouseDown(foundation::Float2{10.0f, 10.0f}, MouseButton::Left); // row 0 anchors
    d->InjectMouseUp(foundation::Float2{10.0f, 10.0f}, MouseButton::Left);
    const foundation::u32 shift = static_cast<foundation::u32>(KeyModShift);
    d->InjectMouseDown(foundation::Float2{10.0f, 70.0f}, MouseButton::Left, shift); // row 3
    d->InjectMouseUp(foundation::Float2{10.0f, 70.0f}, MouseButton::Left, shift);

    CHECK(list->SelectedItems().Size() == 4); // rows 0,1,2,3
    CHECK(list->IsItemSelected(1));
    CHECK(list->IsItemSelected(3));
}

TEST_CASE("itemview: single-selection mode ignores Ctrl (stays single)")
{
    StringListModel model(MakeItems(10));
    auto root = Make<SceneNode>();
    root->SetSize(foundation::Float2{300.0f, 300.0f});
    auto list = Make<ListView>();
    list->SetSize(foundation::Float2{200.0f, 120.0f});
    list->SetRowHeight(20.0f);
    root->AddChild(list.Get()); // default SelectionMode::Single
    list->SetModel(&model);
    EventDispatcher* d = root->GetEventDispatcher();
    const foundation::u32 ctrl = static_cast<foundation::u32>(KeyModCtrl);

    d->InjectMouseDown(foundation::Float2{10.0f, 10.0f}, MouseButton::Left);
    d->InjectMouseUp(foundation::Float2{10.0f, 10.0f}, MouseButton::Left);
    d->InjectMouseDown(foundation::Float2{10.0f, 50.0f}, MouseButton::Left, ctrl); // ctrl ignored
    d->InjectMouseUp(foundation::Float2{10.0f, 50.0f}, MouseButton::Left, ctrl);
    CHECK(list->SelectedItems().Size() == 1);
    CHECK(list->GetSelectedRow() == 2);
}
