// Smoke test for the toolkit StringEditor: value round-trip + EditText submit drives the setter.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
import draconic.foundation;
import draconic.ui;
import draconic.ui.toolkit;

using namespace draconic::ui;
using namespace draconic::ui::toolkit;
using namespace draconic::foundation;
namespace foundation = draconic::foundation;

TEST_CASE("toolkit-stringeditor: RoundTripAndSubmit")
{
    String observed;
    auto ed = foundation::MakeRef<StringEditor>(
        foundation::DefaultAllocator(), StringView(u8"Label"), StringView(u8"hello"),
        Function<void(StringView)>{[&observed](StringView v) { observed = String(v); }});

    CHECK(ed->Value() == StringView(u8"hello"));

    auto* et = foundation::Cast<EditText>(ed->EditorView());
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
