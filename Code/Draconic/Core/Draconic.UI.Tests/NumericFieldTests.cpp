// Ported from Sedulous.UI.Tests/src/NumericFieldTests.bf (faithful). Beef get/set properties -> methods
// (nf->SetMin/SetValue/Value()); [Friend]mText -> Text(); [Friend]mBehavior -> Behavior(). The formatting
// test needs no font (UpdateText uses foundation::FormatFixed).
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
import draconic.foundation;
import draconic.ui;
#include "TestHelpers.h"

using namespace draconic::ui;
using namespace draconic::ui::tests;
using namespace draconic::foundation;
namespace foundation = draconic::foundation;

static foundation::RefPtr<RootView> MakeRoot()
{
    return foundation::MakeRef<RootView>(foundation::DefaultAllocator());
}
static foundation::RefPtr<NumericField> MakeField()
{
    return foundation::MakeRef<NumericField>(foundation::DefaultAllocator());
}

TEST_CASE("numeric-field: ValueClampingMinMax")
{
    auto nf = MakeField();
    nf->SetMin(0);
    nf->SetMax(100);
    nf->SetValue(150);
    CHECK(nf->Value() == 100);
    nf->SetValue(-10);
    CHECK(nf->Value() == 0);
    nf->SetValue(50);
    CHECK(nf->Value() == 50);
}

TEST_CASE("numeric-field: StepIncrement")
{
    auto nf = MakeField();
    nf->SetMin(0);
    nf->SetMax(100);
    nf->SetStep(5);
    nf->SetValue(10);
    nf->Increment();
    CHECK(nf->Value() == 15);
    nf->Decrement();
    CHECK(nf->Value() == 10);
}

TEST_CASE("numeric-field: StepClamps")
{
    auto nf = MakeField();
    nf->SetMin(0);
    nf->SetMax(10);
    nf->SetStep(5);
    nf->SetValue(8);
    nf->Increment();
    CHECK(nf->Value() == 10); // clamped
}

TEST_CASE("numeric-field: OnValueChangedFires")
{
    auto nf = MakeField();
    nf->SetMin(0);
    nf->SetMax(100);

    bool fired = false;
    f64 firedValue = 0;
    nf->OnValueChanged.Add(
        Event<void(NumericField*, f64)>::Handler{[&fired, &firedValue](NumericField*, f64 val)
                                                 {
                                                     fired = true;
                                                     firedValue = val;
                                                 }});

    nf->SetValue(42);
    CHECK(fired);
    CHECK(firedValue == 42);
}

TEST_CASE("numeric-field: DecimalPlacesFormatting")
{
    UIContext ctx;
    auto root = MakeRoot();
    Init(ctx, root.Get());
    auto nf = MakeField();
    nf->SetDecimalPlaces(2);
    nf->SetMin(0);
    nf->SetMax(100);
    root->AddView(nf.Get());
    LayoutPass(ctx, root.Get());

    nf->SetValue(3.14159); // clamped in-range, formatted to 2 places

    const StringView text = nf->Text();
    usize dotIdx = text.Size();
    for (usize i = 0; i < text.Size(); ++i)
    {
        if (text[i] == static_cast<utf8char>('.'))
        {
            dotIdx = i;
            break;
        }
    }
    CHECK(dotIdx != text.Size());          // contains '.'
    CHECK(text.Size() - dotIdx - 1 == 2u); // exactly 2 decimals
    CHECK(text == u8"3.14");
}

TEST_CASE("numeric-field: InputFilterRejectsLetters")
{
    UIContext ctx;
    auto root = MakeRoot();
    Init(ctx, root.Get());
    auto nf = MakeField();
    nf->SetMin(0);
    nf->SetMax(100);
    root->AddView(nf.Get());
    LayoutPass(ctx, root.Get());

    nf->Behavior().HandleKeyDown(KeyCode::A, KeyModifiers::Ctrl); // select all
    nf->Behavior().HandleTextInput(U'5');
    nf->Behavior().HandleTextInput(U'a'); // rejected by the digits/'-'/'.' filter
    nf->Behavior().HandleTextInput(U'3');

    CHECK(nf->Text() == u8"53");
}

TEST_CASE("numeric-field: DefaultValue")
{
    auto nf = MakeField();
    CHECK(nf->Value() == 0);
    CHECK(nf->Min() == 0);
    CHECK(nf->Max() == 100);
    CHECK(nf->Step() == 1);
}

TEST_CASE("numeric-field: ShowSpinButtonsDefault")
{
    auto nf = MakeField();
    CHECK(nf->ShowSpinButtons.Value() == true);
}

TEST_CASE("numeric-field: SelectAllOnFocus")
{
    // Focusing the field (tab or click) selects the whole value for a replacing edit.
    UIContext ctx;
    auto root = MakeRoot();
    Init(ctx, root.Get());
    auto nf = MakeField();
    nf->SetMin(0);
    nf->SetMax(1000);
    nf->SetValue(123);
    root->AddView(nf.Get());
    LayoutPass(ctx, root.Get());

    CHECK(!nf->Behavior().HasSelection());
    ctx.GetFocusManager()->SetFocus(nf.Get());
    CHECK(nf->IsFocused());
    CHECK(nf->Behavior().HasSelection());
    CHECK(nf->Behavior().SelectionStart() == 0);
    CHECK(nf->Behavior().SelectionLength() == static_cast<i32>(nf->Text().Size()));
}
