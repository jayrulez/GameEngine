// Smoke test for the toolkit PropertyGrid: add editors, query by name/count, remove and clear.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
import draconic.foundation;
import draconic.ui;
import draconic.ui.toolkit;

using namespace draconic::ui;
using namespace draconic::ui::toolkit;
using namespace draconic::foundation;
namespace foundation = draconic::foundation;

TEST_CASE("toolkit-propertygrid: AddQueryRemoveClear")
{
    auto grid = foundation::MakeRef<PropertyGrid>(foundation::DefaultAllocator());
    // Constructed with a ScrollView child.
    CHECK(grid->ChildCount() == 1u);
    CHECK(grid->PropertyCount() == 0u);

    grid->AddProperty(
        foundation::MakeRef<BoolEditor>(foundation::DefaultAllocator(), StringView(u8"Visible"), true));
    grid->AddProperty(
        foundation::MakeRef<FloatEditor>(foundation::DefaultAllocator(), StringView(u8"Mass"), 1.0));
    grid->AddProperty(foundation::MakeRef<IntEditor>(foundation::DefaultAllocator(), StringView(u8"Layer"), 0));
    CHECK(grid->PropertyCount() == 3u);

    PropertyEditor* mass = grid->GetProperty(StringView(u8"Mass"));
    REQUIRE(mass != nullptr);
    CHECK(mass->Name() == StringView(u8"Mass"));
    CHECK(grid->GetProperty(StringView(u8"Nope")) == nullptr);

    grid->RemoveProperty(StringView(u8"Layer"));
    CHECK(grid->PropertyCount() == 2u);
    CHECK(grid->GetProperty(StringView(u8"Layer")) == nullptr);

    grid->Clear();
    CHECK(grid->PropertyCount() == 0u);
}

TEST_CASE("toolkit-propertygrid: CategoriesAndDisplayName")
{
    auto grid = foundation::MakeRef<PropertyGrid>(foundation::DefaultAllocator());
    auto ed = foundation::MakeRef<BoolEditor>(foundation::DefaultAllocator(), StringView(u8"CastsShadows"),
                                        false, Function<void(bool)>{}, StringView(u8"Rendering"));
    ed->SetDisplayName(StringView(u8"Casts Shadows"));
    CHECK(ed->DisplayName() == StringView(u8"Casts Shadows"));
    CHECK(ed->Category() == StringView(u8"Rendering"));
    grid->AddProperty(Move(ed));
    CHECK(grid->PropertyCount() == 1u);
    CHECK(grid->PropertyAt(0)->Category() == StringView(u8"Rendering"));
}

TEST_CASE("toolkit-propertyeditor: TooltipAndRowVisibility")
{
    auto ed = foundation::MakeRef<FloatEditor>(foundation::DefaultAllocator(), StringView(u8"Turbidity"), 3.0);
    CHECK(ed->Tooltip().IsEmpty());
    ed->SetTooltip(StringView(u8"Preetham haze"));
    CHECK(ed->Tooltip() == StringView(u8"Preetham haze"));

    // Visibility state applies to the wired row view (PropertyGrid wires it on build).
    auto row = foundation::MakeRef<Label>(foundation::DefaultAllocator());
    CHECK(ed->RowVisible());
    ed->SetRowVisible(false); // before wiring: state only
    CHECK(!ed->RowVisible());
    ed->SetRowView(row.Get()); // wiring applies the current state
    CHECK(row->Visibility == VisibilityValue::Gone);
    ed->SetRowVisible(true);
    CHECK(row->Visibility == VisibilityValue::Visible);
    CHECK(ed->RowVisible());
}

TEST_CASE("toolkit-propertyeditor: display-name changes reach the bound label sink")
{
    // PropertyGrid binds each row's label view through BindDisplayNameSink so a later
    // SetDisplayName (e.g. the inspector's prefab-override dot) updates the LIVE label
    // instead of a string nobody re-reads.
    auto editor = foundation::MakeRef<ButtonEditor>(
        foundation::DefaultAllocator(), StringView(u8"Revert to Prefab"), foundation::Function<void()>{});
    String seen;
    editor->BindDisplayNameSink([&seen](StringView text) { seen = String(text); });
    editor->SetDisplayName(StringView(u8"Revert to Prefab \u25cf"));
    CHECK(seen == StringView(u8"Revert to Prefab \u25cf"));
    CHECK(editor->DisplayName() == StringView(u8"Revert to Prefab \u25cf"));
}
