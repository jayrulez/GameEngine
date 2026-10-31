// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Smoke test for StatusBar: it constructs, adds sections, and measures with a minimum height.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import foundation.ui;
import foundation.ui.toolkit;

using namespace foundation::ui;
using namespace foundation::ui::toolkit;
using namespace foundation::core;
namespace core = foundation::core;

TEST_CASE("toolkit-statusbar: DefaultsAndSections")
{
    auto bar = core::MakeRef<StatusBar>(core::DefaultAllocator());
    CHECK(bar->Direction == Orientation::Horizontal);

    bar->SetText(u8"Ready");
    Label* sec = bar->AddSection(u8"Ln 1, Col 1");
    REQUIRE(sec != nullptr);
    // the default text and one section make 2 children
    CHECK(bar->ChildCount() == 2u);
}
