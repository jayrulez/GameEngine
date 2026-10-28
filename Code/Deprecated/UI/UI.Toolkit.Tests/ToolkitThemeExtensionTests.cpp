// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Smoke test for ToolkitThemeExtension: Apply() populates a StyleSheet with rules for the toolkit
// controls, for both a dark and a light palette (the isDark branch flips on p.Background.r < 0.5).
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import foundation.ui;
import foundation.ui.toolkit;

using namespace foundation::ui;
using namespace foundation::ui::toolkit;
using namespace foundation::core;

TEST_CASE("toolkit-themeextension: AppliesRulesForBothPalettes")
{
    ToolkitThemeExtension ext;

    // Dark palette (default) -> isDark branch.
    {
        RefPtr<StyleSheet> sheetRef = MakeRef<StyleSheet>(DefaultAllocator());
        StyleSheet& sheet = *sheetRef;
        const usize before = sheet.RuleCount();
        ext.Apply(sheet, ThemePalette::Dark());
        CHECK(sheet.RuleCount() > before);
    }

    // Light palette -> the !isDark branch of every control block runs without crashing.
    {
        RefPtr<StyleSheet> sheetRef = MakeRef<StyleSheet>(DefaultAllocator());
        StyleSheet& sheet = *sheetRef;
        ext.Apply(sheet, ThemePalette::Light());
        CHECK(sheet.RuleCount() > 0);
    }
}
