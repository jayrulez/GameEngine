// Draconic GUI - :theme partition
//
// A theme is just a default stylesheet: built-in CSS that styles the standard widget tags plus
// their pseudo-element parts (slider::fill, window::title, scrollbar::thumb, checkbox::mark,
// ...). Parse one with CSSParser and set it on a StyleManager (optionally with an
// IResourceProvider for background-image/font-family). Switching themes is just swapping the
// sheet - the StyleManager already hot-reloads.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.gui:theme;

import draconic.foundation; // StringView

using namespace draconic::foundation;
namespace foundation = draconic::foundation;

export namespace draconic::gui
{
    // Built-in dark theme.
    [[nodiscard]] inline foundation::StringView DefaultDarkThemeCSS()
    {
        return foundation::StringView(u8R"(
            .panel    { background-color: #14161c; }
            label, menuitem, menusubmenu { color: #d2d8e0; }
            button    { background-color: #33507a; color: #f4f7fb; padding: 9; transition: opacity 0.15s; }
            button:hover  { opacity: 0.9; }
            button:active { opacity: 0.78; }
            .tab          { background-color: #23262e; color: #c2c9d4; }
            .tab.selected { background-color: #33507a; color: #f4f7fb; }
            textfield { background-color: #20242b; color: #f0f4f8; padding: 6; }
            listbox   { background-color: #14161a; }
            listbox::selection { background-color: #2f5f9e; }
            listview  { background-color: #14161a; color: #d2d8e0; } /* color propagates to rows */
            listview::selection { background-color: #2f5f9e; }
            tableview { background-color: #14161a; color: #d2d8e0; } /* color -> body cells */
            tableview::selection { background-color: #2f5f9e; }
            tableheader { background-color: #23262e; }
            tableheadercell { color: #c2c9d4; }
            treeview  { background-color: #14161a; color: #d2d8e0; } /* color -> rows */
            treeview::selection { background-color: #2f5f9e; }
            combobox  { background-color: #20242b; color: #eef2f7; padding: 4; }
            scrollview { background-color: #14161a; }
            scrollbar  { background-color: #1e2128; }
            scrollbar::thumb { background-color: #4a505c; }
            window     { background-color: #1a1d24; }
            window::title { background-color: #2b3444; }
            window::grip  { background-color: #3a4353; }
            messagebox { background-color: #1a1d24; }
            messagebox::title { background-color: #2b3444; }
            .dialogbutton { background-color: #33507a; color: #f4f7fb; border-radius: 4; transition: opacity 0.15s; }
            .dialogbutton:hover { opacity: 0.9; }
            menu       { background-color: #1a1d24; }
            menuitem::highlight, menusubmenu::highlight { background-color: #2f5f9e; }
            menuseparator::line { background-color: #3a3f4b; }
            menubar    { background-color: #22262f; }
            .menubutton          { background-color: #22262f; color: #d2d8e0; padding: 6; }
            .menubutton:hover    { background-color: #2f3644; }
            .menubutton.selected { background-color: #33507a; color: #f4f7fb; }
            tabwidget  { background-color: #1a1d24; }
            slider::track { background-color: #2b2f37; }
            slider::fill  { background-color: #4a90d9; }
            slider::thumb { background-color: #cfd6e0; }
            checkbox::box  { background-color: #8a93a2; }
            checkbox::mark { background-color: #4a90d9; }
            radio::ring { background-color: #8a93a2; }
            radio::dot  { background-color: #4a90d9; }
            progressbar::track { background-color: #2b2f37; }
            progressbar::fill  { background-color: #4a90d9; }
        )");
    }

    // Built-in light theme (same structure, light palette).
    [[nodiscard]] inline foundation::StringView DefaultLightThemeCSS()
    {
        return foundation::StringView(u8R"(
            .panel    { background-color: #dde3ec; }
            label, menuitem, menusubmenu { color: #1c2530; }
            button    { background-color: #cdd8ea; color: #17202b; padding: 9; transition: opacity 0.15s; }
            button:hover  { opacity: 0.9; }
            button:active { opacity: 0.78; }
            .tab          { background-color: #cfd6e2; color: #3a4453; }
            .tab.selected { background-color: #ffffff; color: #17202b; }
            textfield { background-color: #ffffff; color: #10161d; padding: 6; }
            listbox   { background-color: #f2f4f8; }
            listbox::selection { background-color: #bcd6f4; }
            listview  { background-color: #f2f4f8; color: #1c2530; } /* color propagates to rows */
            listview::selection { background-color: #bcd6f4; }
            tableview { background-color: #f2f4f8; color: #1c2530; } /* color -> body cells */
            tableview::selection { background-color: #bcd6f4; }
            tableheader { background-color: #cfd6e2; }
            tableheadercell { color: #3a4453; }
            treeview  { background-color: #f2f4f8; color: #1c2530; } /* color -> rows */
            treeview::selection { background-color: #bcd6f4; }
            combobox  { background-color: #ffffff; color: #10161d; padding: 4; }
            scrollview { background-color: #f2f4f8; }
            scrollbar  { background-color: #d5dbe4; }
            scrollbar::thumb { background-color: #a3abb8; }
            window     { background-color: #e6eaf1; }
            window::title { background-color: #c4cdda; }
            window::grip  { background-color: #b0b9c8; }
            messagebox { background-color: #e6eaf1; }
            messagebox::title { background-color: #c4cdda; }
            .dialogbutton { background-color: #cdd8ea; color: #17202b; border-radius: 4; transition: opacity 0.15s; }
            .dialogbutton:hover { opacity: 0.9; }
            menu       { background-color: #f2f4f8; }
            menuitem::highlight, menusubmenu::highlight { background-color: #9cc0ef; }
            menuseparator::line { background-color: #c2c8d2; }
            menubar    { background-color: #d0d6e0; }
            .menubutton          { background-color: #d0d6e0; color: #1c2530; padding: 6; }
            .menubutton:hover    { background-color: #bcc4d2; }
            .menubutton.selected { background-color: #9cc0ef; color: #17202b; }
            tabwidget  { background-color: #e6eaf1; }
            slider::track { background-color: #c2cad6; }
            slider::fill  { background-color: #2f6fb0; }
            slider::thumb { background-color: #3a4658; }
            checkbox::box  { background-color: #5a6474; }
            checkbox::mark { background-color: #2f6fb0; }
            radio::ring { background-color: #5a6474; }
            radio::dot  { background-color: #2f6fb0; }
            progressbar::track { background-color: #c2cad6; }
            progressbar::fill  { background-color: #2f6fb0; }
        )");
    }
}
