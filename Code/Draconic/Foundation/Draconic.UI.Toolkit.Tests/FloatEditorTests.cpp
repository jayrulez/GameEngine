// Smoke test for the toolkit FloatEditor: value round-trip + NumericField change drives the setter.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
import draconic.foundation;
import draconic.ui;
import draconic.ui.toolkit;

using namespace draconic::ui;
using namespace draconic::ui::toolkit;
using namespace draconic::foundation;
namespace foundation = draconic::foundation;

TEST_CASE("toolkit-floateditor: RoundTripAndFieldChange")
{
    f64 observed = 0.0;
    auto ed = foundation::MakeRef<FloatEditor>(foundation::DefaultAllocator(), StringView(u8"Scale"), 1.0, 0.0,
                                         10.0, 0.1, 3,
                                         Function<void(f64)>{[&observed](f64 v) { observed = v; }});

    CHECK(ed->Value() == doctest::Approx(1.0));

    auto* field = foundation::Cast<NumericField>(ed->EditorView());
    REQUIRE(field != nullptr);
    CHECK(field->DecimalPlaces() == 3);

    field->SetValue(2.5);
    CHECK(ed->Value() == doctest::Approx(2.5));
    CHECK(observed == doctest::Approx(2.5));

    ed->SetValue(4.25);
    CHECK(field->Value() == doctest::Approx(4.25));
}
