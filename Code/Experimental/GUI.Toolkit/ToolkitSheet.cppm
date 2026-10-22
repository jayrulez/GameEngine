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
            propertygrid { background-color: var(--propertygrid-background, var(--surface)); color: var(--text); }
            propertycategory { background-color: var(--propertygrid-category, var(--surface-header)); color: var(--text-strong);
                               font-weight: bold; }
            propertyrow:hover { background-color: var(--propertygrid-row-hover, var(--row-hover)); }
            propertyname { color: var(--propertygrid-name, var(--text-dim)); }
            propertyrowcontent.changed > propertyname { color: var(--propertygrid-changed-name, var(--text-strong)); font-weight: bold; }
            propertyrowcontent::marker { background-color: var(--propertygrid-changed, var(--accent)); }
            propertyeditor.unknown label { color: var(--text-dim); font-style: italic; }
            propertyeditor.flags button { background-color: var(--propertygrid-flags, var(--control)); color: var(--text-strong);
                                          padding: var(--space-s); text-align: start; }
            propertyeditor.floats > label.component { color: var(--propertygrid-component, var(--text-dim)); font-size: var(--font-small); }
            propertyeditor.list > label.count { color: var(--text-dim); }
            propertyeditor.list > button.add { background-color: var(--propertygrid-add, var(--control)); color: var(--text-strong);
                                               padding: 2px 10px; border-radius: var(--radius-small); }
            propertycolorpopup { background-color: var(--propertygrid-popup, var(--surface-raised)); border-radius: var(--radius-small);
                                 box-shadow: 0 4px 12px var(--shadow-color); }
            propertyflagslist { background-color: var(--propertygrid-popup, var(--surface-raised)); border-radius: var(--radius-small);
                                box-shadow: 0 4px 12px var(--shadow-color); }
            timeruler { background-color: var(--timeline-ruler, var(--surface-header)); color: var(--text-dim); font-size: var(--font-small); }
            timeruler::tick { background-color: var(--timeline-tick, var(--text-dim)); }
            timeruler::playhead { background-color: var(--timeline-playhead, var(--accent)); }
            toolbar > separator { thickness: 1px; margin: 3px 3px; background-color: var(--toolbar-separator, var(--separator)); }
        )");
    }
}
