// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Smoke test for the toolkit Float3Editor: three-field row + value round-trip; a field drives the setter.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import foundation.ui;
import foundation.ui.toolkit;

using namespace foundation::ui;
using namespace foundation::ui::toolkit;
using namespace foundation::core;
namespace core = foundation::core;

TEST_CASE("toolkit-float3editor: RowAndFieldDrive")
{
    Float3 observed{0, 0, 0};
    auto ed = core::MakeRef<Float3Editor>(
        core::DefaultAllocator(), StringView(u8"Position"), Float3{1.0f, 2.0f, 3.0f}, -1000.0f,
        1000.0f, 0.1f, Function<void(Float3)>{[&observed](Float3 v) { observed = v; }});

    CHECK(ed->Value().x == doctest::Approx(1.0f));
    CHECK(ed->Value().z == doctest::Approx(3.0f));

    auto* row = core::Cast<FlexLayout>(ed->EditorView());
    REQUIRE(row != nullptr);
    CHECK(row->ChildCount() == 3u);

    auto* yField = core::Cast<NumericField>(row->GetChildAt(1));
    REQUIRE(yField != nullptr);
    yField->SetValue(9.0);
    CHECK(ed->Value().y == doctest::Approx(9.0f));
    CHECK(observed.y == doctest::Approx(9.0f));

    ed->SetValue(Float3{4.0f, 5.0f, 6.0f});
    CHECK(yField->Value() == doctest::Approx(5.0));
}
