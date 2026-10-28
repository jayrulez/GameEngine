// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Regression: the "property-field" style class (the editor sizes property-grid fields through
// it) must reach the NumericFields INSIDE every toolkit property editor. Float3Editor & friends
// have their own nested field subclasses - a field created without the class silently keeps the
// theme's default size (the "labels shrank but the numbers didn't" bug).
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import foundation.ui;
import foundation.ui.toolkit;

using namespace foundation::core;
using namespace foundation::ui;
namespace ui = foundation::ui;

namespace
{
    void WalkCheckFields(View* v, i32& fieldCount)
    {
        if (auto* field = Cast<NumericField>(v))
        {
            ++fieldCount;
            CHECK(field->HasClass(u8"property-field"));
            CHECK(field->ResolveStyleFloat(StyleProperty::FontSize, 14.0f) ==
                  doctest::Approx(10.0f));
        }
        if (auto* group = Cast<ViewGroup>(v))
        {
            for (usize i = 0; i < group->ChildCount(); ++i)
            {
                WalkCheckFields(group->GetChildAt(i), fieldCount);
            }
        }
    }
}

TEST_CASE("style: property-field class overrides the type font size on every editor's fields")
{
    UIContext ctx{DefaultAllocator()};
    auto root = MakeRef<RootView>(DefaultAllocator());
    root->ViewportSize = Float2{800, 600};
    ctx.AddRootView(root.Get());

    RefPtr<StyleSheet> sheet = DarkTheme::Create(DefaultAllocator());
    sheet->ForClass(u8"property-field").Set(StyleProperty::FontSize, 10.0f);
    ctx.SetStyleSheet(sheet);

    auto floatEd = MakeRef<ui::toolkit::FloatEditor>(DefaultAllocator(), StringView(u8"f"), 1.0f);
    auto intEd = MakeRef<ui::toolkit::IntEditor>(DefaultAllocator(), StringView(u8"i"), 1);
    auto f2Ed =
        MakeRef<ui::toolkit::Float2Editor>(DefaultAllocator(), StringView(u8"v2"), Float2{});
    auto f3Ed =
        MakeRef<ui::toolkit::Float3Editor>(DefaultAllocator(), StringView(u8"v3"), Float3{});
    auto f4Ed =
        MakeRef<ui::toolkit::Float4Editor>(DefaultAllocator(), StringView(u8"v4"), Float4{});
    auto vec3 = MakeRef<ui::toolkit::Vector3Field>(DefaultAllocator());
    root->AddView(floatEd->EditorView());
    root->AddView(intEd->EditorView());
    root->AddView(f2Ed->EditorView());
    root->AddView(f3Ed->EditorView());
    root->AddView(f4Ed->EditorView());
    root->AddView(vec3.Get());

    i32 fieldCount = 0;
    WalkCheckFields(root.Get(), fieldCount);
    CHECK(fieldCount >= 1 + 1 + 2 + 3 + 4 + 3); // every editor contributed its fields
}
