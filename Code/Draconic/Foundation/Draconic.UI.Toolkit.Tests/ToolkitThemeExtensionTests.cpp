// Smoke test for ToolkitThemeExtension: Apply() populates a StyleSheet with rules for the toolkit
// controls, for both a dark and a light palette (the isDark branch flips on p.Background.r < 0.5).
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
import draconic.foundation;
import draconic.ui;
import draconic.ui.toolkit;

using namespace draconic::ui;
using namespace draconic::ui::toolkit;
using namespace draconic::foundation;

TEST_CASE("toolkit-themeextension: AppliesRulesForBothPalettes")
{
    ToolkitThemeExtension ext;

    // Dark palette (default) -> isDark branch.
    {
        StyleSheet sheet;
        const usize before = sheet.RuleCount();
        ext.Apply(sheet, ThemePalette::Dark());
        CHECK(sheet.RuleCount() > before);
    }

    // Light palette -> the !isDark branch of every control block runs without crashing.
    {
        StyleSheet sheet;
        ext.Apply(sheet, ThemePalette::Light());
        CHECK(sheet.RuleCount() > 0);
    }
}
