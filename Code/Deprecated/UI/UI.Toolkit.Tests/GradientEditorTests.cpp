// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Smoke test for the toolkit GradientEditor: set stops, read them back, update a stop color.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import foundation.ui;
import foundation.ui.toolkit;

using namespace foundation::ui;
using namespace foundation::ui::toolkit;
using namespace foundation::core;
namespace core = foundation::core;

TEST_CASE("toolkit-gradienteditor: SetStopsAndUpdate")
{
    auto ed = core::MakeRef<GradientEditor>(core::DefaultAllocator());
    CHECK(ed->StopCount() == 0);
    CHECK(ed->SelectedIndex() == -1);

    GradientEditor::Stop stops[2] = {
        GradientEditor::Stop{0.0f, Float4{0, 0, 0, 1}},
        GradientEditor::Stop{1.0f, Float4{1, 1, 1, 1}},
    };
    ed->SetStops(Span<const GradientEditor::Stop>(stops, 2));
    CHECK(ed->StopCount() == 2);

    const GradientEditor::Stop s0 = ed->GetStop(0);
    CHECK(s0.Time == doctest::Approx(0.0f));

    // Track that UpdateStopColor fires OnStopChanged.
    i32 changedIdx = -99;
    ed->OnStopChanged.Add([&changedIdx](i32 idx) { changedIdx = idx; });
    ed->UpdateStopColor(1, Float4{1, 0, 0, 1});
    CHECK(changedIdx == 1);
    CHECK(ed->GetStop(1).Color.x == doctest::Approx(1.0f));
    CHECK(ed->GetStop(1).Color.y == doctest::Approx(0.0f));
}
