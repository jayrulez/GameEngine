// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Smoke test for the toolkit EnumEditor: value round-trip + ComboBox selection drives the setter.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import foundation.ui;
import foundation.ui.toolkit;

using namespace foundation::ui;
using namespace foundation::ui::toolkit;
using namespace foundation::core;
namespace core = foundation::core;

TEST_CASE("toolkit-enumeditor: RoundTripAndSelection")
{
    i32 observed = -1;
    const StringView items[] = {StringView(u8"Opaque"), StringView(u8"Cutout"),
                                StringView(u8"Transparent")};
    auto ed = core::MakeRef<EnumEditor>(core::DefaultAllocator(), StringView(u8"Blend"), 0,
                                        Span<const StringView>(items, 3),
                                        Function<void(i32)>{[&observed](i32 v) { observed = v; }});

    CHECK(ed->Value() == 0);

    auto* combo = core::Cast<ComboBox>(ed->EditorView());
    REQUIRE(combo != nullptr);

    combo->SetSelectedIndex(2);
    CHECK(ed->Value() == 2);
    CHECK(observed == 2);

    ed->SetValue(1);
    CHECK(combo->SelectedIndex() == 1);
}
