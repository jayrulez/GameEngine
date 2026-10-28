// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - :theme_extension partition
//
// Lets external libraries inject style rules into a theme StyleSheet after base theme init.
// Ported from Sedulous.UI/src/Styling/IThemeExtension.bf. Injected/held-by-reference (pattern B).

module;
#include "Core/Prelude.h"

export module foundation.ui:theme_extension;

import :style_sheet;
import :theme_palette;

export namespace foundation::ui
{
    class IThemeExtension
    {
    public:
        virtual ~IThemeExtension() = default;

        /// Apply custom style rules to the theme stylesheet (called after base rules are set).
        virtual void Apply(StyleSheet& sheet, ThemePalette palette) = 0;
    };
}
