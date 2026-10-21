// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI.Toolkit - :sheet partition
//
// The toolkit's default sheet (round three's ruling 4): its widgets styled through the theme's
// tokens, so they follow dark, light and contrast; a theme or an application restyles them by
// setting `--toolbar-*` and the rest, over this sheet, not a fork of it.

module;
#include "Core/Prelude.h"

export module experimental.gui.toolkit:sheet;

import foundation.core;

namespace core = foundation::core;

export namespace experimental::gui
{
    /// The toolkit's default sheet, written against the theme's tokens (an application puts it after
    /// the theme in its user agent sheet).
    [[nodiscard]] inline core::StringView ToolkitSheetCSS() noexcept
    {
        return core::StringView(u8R"(
            toolbar { background-color: var(--toolbar-background, var(--chrome-raised)); }
            toolbutton, toolmenubutton, toolbar > overflow {
                background-color: transparent; color: var(--toolbar-text, var(--text));
                padding: var(--toolbar-button-padding, 4px); border-radius: var(--toolbar-button-radius, var(--radius-small));
            }
            toolbutton:hover, toolmenubutton:hover, toolbar > overflow:hover {
                background-color: var(--toolbar-button-hover, var(--control-hover));
            }
            toolbutton:active, toolmenubutton:active, toolbar > overflow:active {
                background-color: var(--toolbar-button-pressed, var(--control-raised));
            }
            toolbutton:checked, toolmenubutton:checked { background-color: var(--toolbar-button-checked, var(--selection)); }
            toolbutton:disabled, toolmenubutton:disabled { opacity: var(--disabled-opacity); }
            toolbutton:focus-visible, toolmenubutton:focus-visible, toolbar > overflow:focus-visible {
                box-shadow: 0 0 0 var(--focus-width) var(--focus-ring);
            }
            toolbutton > icon, toolmenubutton > icon { width: var(--toolbar-icon, 16px); height: var(--toolbar-icon, 16px); }
            toolbutton > text, toolmenubutton > text { padding: 0 2px; }
            statusbar { background-color: var(--statusbar-background, var(--chrome)); color: var(--statusbar-text, var(--text-dim));
                        font-size: var(--font-small); }
            statusbar label { color: var(--statusbar-text, var(--text-dim)); }
            statusbar label.message { color: var(--statusbar-message, var(--text)); }
            toolbar > separator { thickness: 1px; margin: 3px 3px; background-color: var(--toolbar-separator, var(--separator)); }
        )");
    }
}
