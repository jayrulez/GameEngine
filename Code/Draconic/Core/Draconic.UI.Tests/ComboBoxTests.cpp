// Ported from Sedulous.UI.Tests/src/ComboBoxTests.bf (faithful). Beef get/set props -> methods
// (cb->SetSelectedIndex / SelectedIndex()); item/selection/event logic only - no popup, no font.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
import draconic.foundation;
import draconic.ui;

using namespace draconic::ui;
using namespace draconic::foundation;
namespace foundation = draconic::foundation;

static foundation::RefPtr<ComboBox> MakeCombo()
{
    return foundation::MakeRef<ComboBox>(foundation::DefaultAllocator());
}

TEST_CASE("combo-box: AddItem_ReturnsIndex")
{
    auto cb = MakeCombo();
    CHECK(cb->AddItem(u8"A") == 0);
    CHECK(cb->AddItem(u8"B") == 1);
    CHECK(cb->AddItem(u8"C") == 2);
    CHECK(cb->ItemCount() == 3);
}

TEST_CASE("combo-box: RemoveItem_DecreasesCount")
{
    auto cb = MakeCombo();
    cb->AddItem(u8"A");
    cb->AddItem(u8"B");
    cb->AddItem(u8"C");
    cb->RemoveItem(1);
    CHECK(cb->ItemCount() == 2);
}

TEST_CASE("combo-box: ClearItems_EmptiesList")
{
    auto cb = MakeCombo();
    cb->AddItem(u8"A");
    cb->AddItem(u8"B");
    cb->ClearItems();
    CHECK(cb->ItemCount() == 0);
    CHECK(cb->SelectedIndex() == -1);
}

TEST_CASE("combo-box: SelectedIndex_Clamping")
{
    auto cb = MakeCombo();
    cb->AddItem(u8"A");
    cb->AddItem(u8"B");
    cb->SetSelectedIndex(5);
    CHECK(cb->SelectedIndex() == 1); // clamped to max
    cb->SetSelectedIndex(-5);
    CHECK(cb->SelectedIndex() == -1); // clamped to -1
}

TEST_CASE("combo-box: SelectedText_ReturnsCorrect")
{
    auto cb = MakeCombo();
    cb->AddItem(u8"Alpha");
    cb->AddItem(u8"Beta");
    CHECK(cb->SelectedText() == u8""); // no selection
    cb->SetSelectedIndex(1);
    CHECK(cb->SelectedText() == u8"Beta");
}

TEST_CASE("combo-box: OnSelectionChanged_Fires")
{
    auto cb = MakeCombo();
    cb->AddItem(u8"A");
    cb->AddItem(u8"B");
    bool fired = false;
    i32 firedIndex = -1;
    cb->OnSelectionChanged.Add(
        Event<void(ComboBox*, i32)>::Handler{[&fired, &firedIndex](ComboBox*, i32 idx)
                                             {
                                                 fired = true;
                                                 firedIndex = idx;
                                             }});
    cb->SetSelectedIndex(1);
    CHECK(fired);
    CHECK(firedIndex == 1);
}

TEST_CASE("combo-box: IsFocusable")
{
    auto cb = MakeCombo();
    CHECK(cb->IsFocusable);
    CHECK(cb->IsTabStop);
}

TEST_CASE("combo-box: DefaultState_NoSelection")
{
    auto cb = MakeCombo();
    CHECK(cb->SelectedIndex() == -1);
    CHECK(cb->SelectedText() == u8"");
    CHECK(!cb->IsOpen());
    CHECK(cb->ItemCount() == 0);
}
