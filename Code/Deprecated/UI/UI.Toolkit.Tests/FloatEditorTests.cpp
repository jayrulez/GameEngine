// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Smoke test for the toolkit FloatEditor: value round-trip + NumericField change drives the setter.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import foundation.ui;
import foundation.ui.toolkit;

using namespace foundation::ui;
using namespace foundation::ui::toolkit;
using namespace foundation::core;
namespace core = foundation::core;

TEST_CASE("toolkit-floateditor: RoundTripAndFieldChange")
{
    f64 observed = 0.0;
    auto ed = core::MakeRef<FloatEditor>(core::DefaultAllocator(), StringView(u8"Scale"), 1.0, 0.0,
                                         10.0, 0.1, 3,
                                         Function<void(f64)>{[&observed](f64 v) { observed = v; }});

    CHECK(ed->Value() == doctest::Approx(1.0));

    auto* field = core::Cast<NumericField>(ed->EditorView());
    REQUIRE(field != nullptr);
    CHECK(field->DecimalPlaces() == 3);

    field->SetValue(2.5);
    CHECK(ed->Value() == doctest::Approx(2.5));
    CHECK(observed == doctest::Approx(2.5));

    ed->SetValue(4.25);
    CHECK(field->Value() == doctest::Approx(4.25));
}
