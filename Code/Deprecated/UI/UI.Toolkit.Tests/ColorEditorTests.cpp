// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Smoke test for the toolkit ColorEditor: value round-trip through the ColorView swatch.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import foundation.ui;
import foundation.ui.toolkit;

using namespace foundation::ui;
using namespace foundation::ui::toolkit;
using namespace foundation::core;
namespace core = foundation::core;

TEST_CASE("toolkit-coloreditor: RoundTripSwatch")
{
    auto ed = core::MakeRef<ColorEditor>(core::DefaultAllocator(), StringView(u8"Tint"),
                                         core::Color{1.0f, 0.0f, 0.0f, 1.0f});

    CHECK(ed->Value().r == doctest::Approx(1.0f));

    auto* swatch = core::Cast<ColorView>(ed->EditorView());
    REQUIRE(swatch != nullptr);
    CHECK(swatch->Color.Value().r == doctest::Approx(1.0f));

    // External SetValue updates the swatch (opening the picker dialog needs a live UIContext, skipped).
    ed->SetValue(core::Color{0.0f, 0.5f, 1.0f, 1.0f});
    CHECK(swatch->Color.Value().b == doctest::Approx(1.0f));
    CHECK(swatch->Cursor == CursorType::Hand);
}
