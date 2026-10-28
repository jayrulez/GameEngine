// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Smoke test for the toolkit PropertyGrid: add editors, query by name/count, remove and clear.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import foundation.ui;
import foundation.ui.toolkit;

using namespace foundation::ui;
using namespace foundation::ui::toolkit;
using namespace foundation::core;
namespace core = foundation::core;

TEST_CASE("toolkit-propertygrid: AddQueryRemoveClear")
{
    auto grid = core::MakeRef<PropertyGrid>(core::DefaultAllocator());
    // Constructed with a ScrollView child.
    CHECK(grid->ChildCount() == 1u);
    CHECK(grid->PropertyCount() == 0u);

    grid->AddProperty(
        core::MakeRef<BoolEditor>(core::DefaultAllocator(), StringView(u8"Visible"), true));
    grid->AddProperty(
        core::MakeRef<FloatEditor>(core::DefaultAllocator(), StringView(u8"Mass"), 1.0));
    grid->AddProperty(core::MakeRef<IntEditor>(core::DefaultAllocator(), StringView(u8"Layer"), 0));
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
    auto grid = core::MakeRef<PropertyGrid>(core::DefaultAllocator());
    auto ed = core::MakeRef<BoolEditor>(core::DefaultAllocator(), StringView(u8"CastsShadows"),
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
    auto ed = core::MakeRef<FloatEditor>(core::DefaultAllocator(), StringView(u8"Turbidity"), 3.0);
    CHECK(ed->Tooltip().IsEmpty());
    ed->SetTooltip(StringView(u8"Preetham haze"));
    CHECK(ed->Tooltip() == StringView(u8"Preetham haze"));

    // Visibility state applies to the wired row view (PropertyGrid wires it on build).
    auto row = core::MakeRef<Label>(core::DefaultAllocator());
    CHECK(ed->RowVisible());
    ed->SetRowVisible(false); // before wiring: state only
    CHECK(!ed->RowVisible());
    ed->SetRowView(row.Get()); // wiring applies the current state
    CHECK(row->Visibility == VisibilityValue::Gone);
    ed->SetRowVisible(true);
    CHECK(row->Visibility == VisibilityValue::Visible);
    CHECK(ed->RowVisible());
}

namespace
{
    // Depth-first search for the category Expander PropertyGrid built for `header` (the grid's
    // internal tree is ScrollView -> FlexLayout -> Expander per category).
    Expander* FindCategoryExpander(View* view, StringView header)
    {
        if (auto* expander = Cast<Expander>(view))
        {
            if (expander->HeaderText() == header)
            {
                return expander;
            }
        }
        if (auto* group = Cast<ViewGroup>(view))
        {
            for (usize i = 0; i < group->ChildCount(); ++i)
            {
                if (Expander* found = FindCategoryExpander(group->GetChildAt(i), header))
                {
                    return found;
                }
            }
        }
        return nullptr;
    }

    RefPtr<BoolEditor> InCategory(StringView name, StringView category)
    {
        return core::MakeRef<BoolEditor>(core::DefaultAllocator(), name, false,
                                         Function<void(bool)>{}, category);
    }
}

TEST_CASE("toolkit-propertygrid: user expansion survives rebuilds of a default-collapsed category")
{
    auto grid = core::MakeRef<PropertyGrid>(core::DefaultAllocator());
    grid->AddProperty(InCategory(u8"A", u8"Bulk"));
    grid->SetCategoryDefaultCollapsed(StringView(u8"Bulk"));

    grid->Measure(BoxConstraints::Tight(400, 600)); // builds the category expanders
    Expander* expander = FindCategoryExpander(grid.Get(), StringView(u8"Bulk"));
    REQUIRE(expander != nullptr);
    CHECK_FALSE(expander->IsExpanded()); // the default-collapsed list applies on first build

    // The user opens the category, then a rebuild happens (a property is added).
    expander->SetIsExpanded(true);
    grid->AddProperty(InCategory(u8"B", u8"Bulk"));
    grid->Measure(BoxConstraints::Tight(400, 600));

    // The rebuilt expander is a NEW view; the remembered state wins over the default.
    expander = FindCategoryExpander(grid.Get(), StringView(u8"Bulk"));
    REQUIRE(expander != nullptr);
    CHECK(expander->IsExpanded());
}

TEST_CASE("toolkit-propertygrid: user collapse survives rebuilds of a normal category")
{
    auto grid = core::MakeRef<PropertyGrid>(core::DefaultAllocator());
    grid->AddProperty(InCategory(u8"A", u8"Main"));

    grid->Measure(BoxConstraints::Tight(400, 600));
    Expander* expander = FindCategoryExpander(grid.Get(), StringView(u8"Main"));
    REQUIRE(expander != nullptr);
    CHECK(expander->IsExpanded()); // no default-collapse: builds expanded

    // The user closes the category, then a rebuild happens.
    expander->SetIsExpanded(false);
    grid->AddProperty(InCategory(u8"B", u8"Main"));
    grid->Measure(BoxConstraints::Tight(400, 600));

    expander = FindCategoryExpander(grid.Get(), StringView(u8"Main"));
    REQUIRE(expander != nullptr);
    CHECK_FALSE(expander->IsExpanded()); // stays collapsed - not reopened by the rebuild
}

TEST_CASE("toolkit-propertyeditor: display-name changes reach the bound label sink")
{
    // PropertyGrid binds each row's label view through BindDisplayNameSink so a later
    // SetDisplayName (e.g. the inspector's prefab-override dot) updates the LIVE label
    // instead of a string nobody re-reads.
    auto editor = core::MakeRef<ButtonEditor>(
        core::DefaultAllocator(), StringView(u8"Revert to Prefab"), core::Function<void()>{});
    String seen;
    editor->BindDisplayNameSink([&seen](StringView text) { seen = String(text); });
    editor->SetDisplayName(StringView(u8"Revert to Prefab \u25cf"));
    CHECK(seen == StringView(u8"Revert to Prefab \u25cf"));
    CHECK(editor->DisplayName() == StringView(u8"Revert to Prefab \u25cf"));
}

namespace
{
    // The expander headed `header`, anywhere in the tree, or null.
    Expander* FindSection(View* view, StringView header)
    {
        if (auto* expander = Cast<Expander>(view); expander != nullptr && expander->HeaderText() == header)
        {
            return expander;
        }
        if (auto* group = Cast<ViewGroup>(view))
        {
            for (usize i = 0; i < group->ChildCount(); ++i)
            {
                if (Expander* found = FindSection(group->GetChildAt(i), header))
                {
                    return found;
                }
            }
        }
        return nullptr;
    }

    // The section a section sits in, through its body, or null at the top level.
    Expander* SectionAround(Expander* section)
    {
        return section->Parent != nullptr ? Cast<Expander>(section->Parent->Parent) : nullptr;
    }

    RefPtr<FloatEditor> Row(StringView name, StringView category)
    {
        return core::MakeRef<FloatEditor>(core::DefaultAllocator(), name, 1.0, 0.0, 10.0, 1.0, 2,
                                          Function<void(f64)>{}, category);
    }
}

// A category can sit inside another's body (a component's behaviours inside the component); a
// parent that is not there, or a loop, leaves it at the top level, and a nested section keeps
// whether it was open across a rebuild (Sedulous 593d0a36).
TEST_CASE("toolkit-propertygrid: a category nests inside its parent's body")
{
    auto grid = core::MakeRef<PropertyGrid>(core::DefaultAllocator());
    grid->AddProperty(Row(u8"a", u8"Script"));
    grid->AddProperty(Row(u8"b", u8"Behavior 1"));
    grid->AddProperty(Row(u8"c", u8"Orphan"));
    grid->AddProperty(Row(u8"e", u8"E"));
    grid->AddProperty(Row(u8"f", u8"F"));
    grid->SetCategoryParent(u8"Behavior 1", u8"Script");
    grid->SetCategoryParent(u8"Orphan", u8"Missing");
    grid->SetCategoryParent(u8"E", u8"F");
    grid->SetCategoryParent(u8"F", u8"E");
    CHECK(grid->CategoryParent(u8"Behavior 1") == StringView(u8"Script"));
    grid->Measure(BoxConstraints::Tight(400, 600));

    Expander* script = FindSection(grid.Get(), u8"Script");
    Expander* behavior = FindSection(grid.Get(), u8"Behavior 1");
    REQUIRE(script != nullptr);
    REQUIRE(behavior != nullptr);
    CHECK(SectionAround(behavior) == script); // inside the parent's body
    CHECK(SectionAround(script) == nullptr);
    CHECK(SectionAround(FindSection(grid.Get(), u8"Orphan")) == nullptr); // missing parent: top
    CHECK(SectionAround(FindSection(grid.Get(), u8"E")) == nullptr);      // a loop: top level
    CHECK(SectionAround(FindSection(grid.Get(), u8"F")) == nullptr);

    // Closed, then rebuilt: it stays closed, where it was.
    behavior->SetIsExpanded(false);
    grid->AddProperty(Row(u8"g", u8"Script"));
    grid->Measure(BoxConstraints::Tight(400, 600));
    Expander* rebuilt = FindSection(grid.Get(), u8"Behavior 1");
    REQUIRE(rebuilt != nullptr);
    CHECK_FALSE(rebuilt->IsExpanded());
    CHECK(SectionAround(rebuilt) == FindSection(grid.Get(), u8"Script"));
}

TEST_CASE("toolkit-propertygrid: a category's header actions read back")
{
    auto grid = core::MakeRef<PropertyGrid>(core::DefaultAllocator());
    auto actions = core::MakeRef<FlexLayout>(core::DefaultAllocator());
    CHECK(grid->GetCategoryHeaderActions(u8"Section") == nullptr);
    grid->SetCategoryHeaderActions(u8"Section", RefPtr<View>(actions.Get()));
    CHECK(grid->GetCategoryHeaderActions(u8"Section") == actions.Get());
    grid->Clear();
    CHECK(grid->GetCategoryHeaderActions(u8"Section") == nullptr);
}
