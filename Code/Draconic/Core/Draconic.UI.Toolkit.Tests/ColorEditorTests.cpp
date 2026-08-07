// Smoke test for the toolkit ColorEditor: value round-trip through the ColorView swatch.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
import draconic.foundation;
import draconic.ui;
import draconic.ui.toolkit;

using namespace draconic::ui;
using namespace draconic::ui::toolkit;
using namespace draconic::foundation;
namespace foundation = draconic::foundation;

TEST_CASE("toolkit-coloreditor: RoundTripSwatch")
{
    auto ed = foundation::MakeRef<ColorEditor>(foundation::DefaultAllocator(), StringView(u8"Tint"),
                                         foundation::Color{1.0f, 0.0f, 0.0f, 1.0f});

    CHECK(ed->Value().r == doctest::Approx(1.0f));

    auto* swatch = foundation::Cast<ColorView>(ed->EditorView());
    REQUIRE(swatch != nullptr);
    CHECK(swatch->Color.Value().r == doctest::Approx(1.0f));

    // External SetValue updates the swatch (opening the picker dialog needs a live UIContext, skipped).
    ed->SetValue(foundation::Color{0.0f, 0.5f, 1.0f, 1.0f});
    CHECK(swatch->Color.Value().b == doctest::Approx(1.0f));
    CHECK(swatch->Cursor == CursorType::Hand);
}
