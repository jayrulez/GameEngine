// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - :theme_extension partition
//
// Lets external libraries inject style rules into a theme StyleSheet after the base theme is built.
// Extensions are registered with and held by reference by ThemeRegistry.

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
