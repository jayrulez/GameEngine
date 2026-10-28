// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - :theme_icons partition
//
// Built-in SVG icon definitions for theme drawable keys. These are string constants compiled into
// the binary - no file loading needed. Themes register these as SVGDrawables for icon style
// properties. Ported from Sedulous.UI/src/Styling/ThemeIcons.bf.
//
// Divergence (language): Beef static get-properties (`=>`) become static methods returning
// StringView; the SVG payloads are UTF-8 raw string literals (char8_t).

module;
#include "Core/Prelude.h"

export module foundation.ui:theme_icons;

import foundation.core; // StringView

using namespace foundation::core;

export namespace foundation::ui
{
    /// Built-in SVG icon markup for theme drawable keys.
    struct ThemeIcons
    {
        /// Checkmark icon for CheckBox.
        static StringView Checkmark()
        {
            return StringView(u8R"(<svg viewBox="0 0 16 16">
  <path d="M3 8 L6.5 11.5 L13 5" fill="none" stroke="white" stroke-width="2"/>
</svg>)");
        }

        /// Down-pointing arrow for ComboBox dropdown.
        static StringView ArrowDown()
        {
            return StringView(u8R"(<svg viewBox="0 0 12 12">
  <path d="M2 4 L6 8 L10 4" fill="none" stroke="white" stroke-width="1.5"/>
</svg>)");
        }

        /// Up-pointing arrow for NumericField increment.
        static StringView ArrowUp()
        {
            return StringView(u8R"(<svg viewBox="0 0 12 12">
  <path d="M2 8 L6 4 L10 8" fill="none" stroke="white" stroke-width="1.5"/>
</svg>)");
        }

        /// Right-pointing chevron for collapsed Expander/TreeView.
        static StringView ChevronRight()
        {
            return StringView(u8R"(<svg viewBox="0 0 10 12">
  <path d="M3 2 L7 6 L3 10" fill="none" stroke="white" stroke-width="1.5"/>
</svg>)");
        }

        /// Down-pointing chevron for expanded Expander/TreeView.
        static StringView ChevronDown()
        {
            return StringView(u8R"(<svg viewBox="0 0 12 10">
  <path d="M2 3 L6 7 L10 3" fill="none" stroke="white" stroke-width="1.5"/>
</svg>)");
        }

        /// Close (X) icon for tab close buttons, panel close buttons.
        static StringView Close()
        {
            return StringView(u8R"(<svg viewBox="0 0 12 12">
  <path d="M2 2 L10 10 M10 2 L2 10" fill="none" stroke="white" stroke-width="1.5"/>
</svg>)");
        }

        /// Plus icon (for add buttons, etc.)
        static StringView Plus()
        {
            return StringView(u8R"(<svg viewBox="0 0 12 12">
  <path d="M6 2 L6 10 M2 6 L10 6" fill="none" stroke="white" stroke-width="1.5"/>
</svg>)");
        }

        /// Minus icon (for remove buttons, etc.)
        static StringView Minus()
        {
            return StringView(u8R"(<svg viewBox="0 0 12 12">
  <path d="M2 6 L10 6" fill="none" stroke="white" stroke-width="1.5"/>
</svg>)");
        }

        /// Square mark for RadioButton (flat themes).
        static StringView RadioMarkSquare()
        {
            return StringView(u8R"(<svg viewBox="0 0 8 8">
  <rect x="1" y="1" width="6" height="6" fill="white"/>
</svg>)");
        }

        /// Round mark for RadioButton (rounded themes).
        static StringView RadioMarkRound()
        {
            return StringView(u8R"(<svg viewBox="0 0 8 8">
  <circle cx="4" cy="4" r="3" fill="white"/>
</svg>)");
        }
    };
}
