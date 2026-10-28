// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Smoke test for the toolkit StringEditor: value round-trip + EditText submit drives the setter.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import foundation.ui;
import foundation.ui.toolkit;

using namespace foundation::ui;
using namespace foundation::ui::toolkit;
using namespace foundation::core;
namespace core = foundation::core;

TEST_CASE("toolkit-stringeditor: RoundTripAndSubmit")
{
    String observed;
    auto ed = core::MakeRef<StringEditor>(
        core::DefaultAllocator(), StringView(u8"Label"), StringView(u8"hello"),
        Function<void(StringView)>{[&observed](StringView v) { observed = String(v); }});

    CHECK(ed->Value() == StringView(u8"hello"));

    auto* et = core::Cast<EditText>(ed->EditorView());
    REQUIRE(et != nullptr);
    CHECK(et->Text() == StringView(u8"hello"));

    // Typing then submitting flows through the editor -> setter + value update.
    et->SetText(StringView(u8"world"));
    et->OnSubmit.Invoke(et);
    CHECK(ed->Value() == StringView(u8"world"));
    CHECK(observed == StringView(u8"world"));

    // External SetValue refreshes the control.
    ed->SetValue(StringView(u8"again"));
    CHECK(et->Text() == StringView(u8"again"));
}
