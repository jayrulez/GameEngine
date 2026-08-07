// Smoke test for the toolkit EnumEditor: value round-trip + ComboBox selection drives the setter.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
import draconic.foundation;
import draconic.ui;
import draconic.ui.toolkit;

using namespace draconic::ui;
using namespace draconic::ui::toolkit;
using namespace draconic::foundation;
namespace foundation = draconic::foundation;

TEST_CASE("toolkit-enumeditor: RoundTripAndSelection")
{
    i32 observed = -1;
    const StringView items[] = {StringView(u8"Opaque"), StringView(u8"Cutout"),
                                StringView(u8"Transparent")};
    auto ed = foundation::MakeRef<EnumEditor>(foundation::DefaultAllocator(), StringView(u8"Blend"), 0,
                                        Span<const StringView>(items, 3),
                                        Function<void(i32)>{[&observed](i32 v) { observed = v; }});

    CHECK(ed->Value() == 0);

    auto* combo = foundation::Cast<ComboBox>(ed->EditorView());
    REQUIRE(combo != nullptr);

    combo->SetSelectedIndex(2);
    CHECK(ed->Value() == 2);
    CHECK(observed == 2);

    ed->SetValue(1);
    CHECK(combo->SelectedIndex() == 1);
}
