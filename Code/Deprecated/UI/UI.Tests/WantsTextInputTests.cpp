// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Tests for WantsTextInput - our addition (Sedulous shell never finished text input): text controls
// return true when focused-and-editable, and UIContext::WantsTextInput() reflects the focused view, so the
// ui.shell bridge can drive the window's IME from focus. Not a port - covered here per the additions rule.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import foundation.ui;
#include "TestHelpers.h"

using namespace foundation::ui;
using namespace foundation::ui::tests;
using namespace foundation::core;
namespace core = foundation::core;

TEST_CASE("wants-text-input: plain view does not want text")
{
    auto v = core::MakeRef<TestView>(core::DefaultAllocator(), 50.0f, 30.0f);
    CHECK(!v->WantsTextInput());
}

TEST_CASE("wants-text-input: EditText wants text unless read-only")
{
    auto edit = core::MakeRef<EditText>(core::DefaultAllocator());
    CHECK(edit->WantsTextInput());
    edit->IsReadOnly.SetValue(true);
    CHECK(!edit->WantsTextInput());
    edit->IsReadOnly.SetValue(false);
    edit->IsEnabled = false;
    CHECK(!edit->WantsTextInput()); // disabled -> no text input
}

TEST_CASE("wants-text-input: NumericField wants text")
{
    auto nf = core::MakeRef<NumericField>(core::DefaultAllocator());
    CHECK(nf->WantsTextInput());
}

TEST_CASE("wants-text-input: UIContext reflects the focused view")
{
    UIContext ctx{DefaultAllocator()};
    auto root = core::MakeRef<RootView>(core::DefaultAllocator());
    Init(ctx, root.Get());

    auto edit = core::MakeRef<EditText>(core::DefaultAllocator());
    auto plain = core::MakeRef<TestView>(core::DefaultAllocator(), 50.0f, 30.0f);
    plain->IsFocusable = true;
    root->AddView(edit.Get());
    root->AddView(plain.Get());

    CHECK(!ctx.WantsTextInput()); // nothing focused

    ctx.GetFocusManager()->SetFocus(edit.Get());
    CHECK(ctx.WantsTextInput()); // EditText focused

    ctx.GetFocusManager()->SetFocus(plain.Get());
    CHECK(!ctx.WantsTextInput()); // non-text view focused

    ctx.GetFocusManager()->ClearFocus();
    CHECK(!ctx.WantsTextInput());
}
