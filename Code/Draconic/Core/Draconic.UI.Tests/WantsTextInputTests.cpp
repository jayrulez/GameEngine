// Tests for WantsTextInput - a Draconic addition (Sedulous shell never finished text input): text controls
// return true when focused-and-editable, and UIContext::WantsTextInput() reflects the focused view, so the
// ui.shell bridge can drive the window's IME from focus. Not a port - covered here per the additions rule.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
import draconic.foundation;
import draconic.ui;
#include "TestHelpers.h"

using namespace draconic::ui;
using namespace draconic::ui::tests;
using namespace draconic::foundation;
namespace foundation = draconic::foundation;

TEST_CASE("wants-text-input: plain view does not want text")
{
    auto v = foundation::MakeRef<TestView>(foundation::DefaultAllocator(), 50.0f, 30.0f);
    CHECK(!v->WantsTextInput());
}

TEST_CASE("wants-text-input: EditText wants text unless read-only")
{
    auto edit = foundation::MakeRef<EditText>(foundation::DefaultAllocator());
    CHECK(edit->WantsTextInput());
    edit->IsReadOnly.SetValue(true);
    CHECK(!edit->WantsTextInput());
    edit->IsReadOnly.SetValue(false);
    edit->IsEnabled = false;
    CHECK(!edit->WantsTextInput()); // disabled -> no text input
}

TEST_CASE("wants-text-input: NumericField wants text")
{
    auto nf = foundation::MakeRef<NumericField>(foundation::DefaultAllocator());
    CHECK(nf->WantsTextInput());
}

TEST_CASE("wants-text-input: UIContext reflects the focused view")
{
    UIContext ctx;
    auto root = foundation::MakeRef<RootView>(foundation::DefaultAllocator());
    Init(ctx, root.Get());

    auto edit = foundation::MakeRef<EditText>(foundation::DefaultAllocator());
    auto plain = foundation::MakeRef<TestView>(foundation::DefaultAllocator(), 50.0f, 30.0f);
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
