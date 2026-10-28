// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Smoke test for the toolkit RangeEditor: builds a Slider + NumericField row; slider drives the setter.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import foundation.ui;
import foundation.ui.toolkit;

using namespace foundation::ui;
using namespace foundation::ui::toolkit;
using namespace foundation::core;
namespace core = foundation::core;

TEST_CASE("toolkit-rangeeditor: RowAndSliderDrive")
{
    f32 observed = 0.0f;
    auto ed = core::MakeRef<RangeEditor>(core::DefaultAllocator(), StringView(u8"Opacity"), 0.5f,
                                         0.0f, 1.0f, 0.0f,
                                         Function<void(f32)>{[&observed](f32 v) { observed = v; }});

    CHECK(ed->Value() == doctest::Approx(0.5f));

    auto* row = core::Cast<FlexLayout>(ed->EditorView());
    REQUIRE(row != nullptr);
    // Slider + NumericField.
    CHECK(row->ChildCount() == 2u);

    auto* slider = core::Cast<Slider>(row->GetChildAt(0));
    REQUIRE(slider != nullptr);
    slider->Value.SetValue(0.75f);
    CHECK(ed->Value() == doctest::Approx(0.75f));
    CHECK(observed == doctest::Approx(0.75f));

    ed->SetValue(0.25f);
    CHECK(slider->Value.Value() == doctest::Approx(0.25f));
}
