// Smoke test for the toolkit RangeEditor: builds a Slider + NumericField row; slider drives the setter.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
import draconic.foundation;
import draconic.ui;
import draconic.ui.toolkit;

using namespace draconic::ui;
using namespace draconic::ui::toolkit;
using namespace draconic::foundation;
namespace foundation = draconic::foundation;

TEST_CASE("toolkit-rangeeditor: RowAndSliderDrive")
{
    f32 observed = 0.0f;
    auto ed = foundation::MakeRef<RangeEditor>(foundation::DefaultAllocator(), StringView(u8"Opacity"), 0.5f,
                                         0.0f, 1.0f, 0.0f,
                                         Function<void(f32)>{[&observed](f32 v) { observed = v; }});

    CHECK(ed->Value() == doctest::Approx(0.5f));

    auto* row = foundation::Cast<FlexLayout>(ed->EditorView());
    REQUIRE(row != nullptr);
    // Slider + NumericField.
    CHECK(row->ChildCount() == 2u);

    auto* slider = foundation::Cast<Slider>(row->GetChildAt(0));
    REQUIRE(slider != nullptr);
    slider->Value.SetValue(0.75f);
    CHECK(ed->Value() == doctest::Approx(0.75f));
    CHECK(observed == doctest::Approx(0.75f));

    ed->SetValue(0.25f);
    CHECK(slider->Value.Value() == doctest::Approx(0.25f));
}
