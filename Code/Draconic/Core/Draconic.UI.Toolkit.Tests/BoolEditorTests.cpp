// Smoke test for the toolkit BoolEditor: value round-trip + CheckBox toggle drives the setter.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
import draconic.foundation;
import draconic.ui;
import draconic.ui.toolkit;

using namespace draconic::ui;
using namespace draconic::ui::toolkit;
using namespace draconic::foundation;
namespace foundation = draconic::foundation;

TEST_CASE("toolkit-booleditor: RoundTripAndToggle")
{
    bool observed = false;
    auto ed =
        foundation::MakeRef<BoolEditor>(foundation::DefaultAllocator(), StringView(u8"Enabled"), false,
                                  Function<void(bool)>{[&observed](bool v) { observed = v; }});

    CHECK(ed->Name() == StringView(u8"Enabled"));
    CHECK(ed->Value() == false);

    auto* cb = foundation::Cast<CheckBox>(ed->EditorView());
    REQUIRE(cb != nullptr);

    // Toggling the checkbox flows through the editor -> setter + value update.
    cb->IsChecked.SetValue(true);
    CHECK(ed->Value() == true);
    CHECK(observed == true);

    // External SetValue updates the control.
    ed->SetValue(false);
    CHECK(cb->IsChecked.Value() == false);
}
