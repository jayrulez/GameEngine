// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell
// Editor::App - :view_mode_toggles partition (implementation).
module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

module editor.app;

import foundation.core;
import foundation.ui;

using namespace foundation::core;
namespace ui = foundation::ui;

namespace editor::app
{
    ViewModeToggles::ViewModeToggles()
    {
        Direction = ui::Orientation::Horizontal;
        Spacing = 2;
        EditorIcons& icons = EditorIcons::Get(); // null drawables before Initialize (tests)
        m_list = MakeToggle(icons.viewList.Get(), u8"List", false);
        m_gridToggle = MakeToggle(icons.viewGrid.Get(), u8"Grid", true);
        m_list->IsChecked.SetValue(true);
        ui::LayoutStyle center;
        center.AlignSelf = ui::Align::Center;
        AddView(m_list.Get(), center);
        AddView(m_gridToggle.Get(), center);
    }

    void ViewModeToggles::SetGridMode(bool grid)
    {
        m_grid = grid;
        m_list->IsChecked.SetValue(!grid);
        m_gridToggle->IsChecked.SetValue(grid);
    }

    RefPtr<ui::ToggleButton> ViewModeToggles::MakeToggle(ui::SVGDrawable* icon, StringView tooltip,
                                                         bool grid)
    {
        auto glyph = MakeRef<ui::DrawableView>(MemoryAllocator(), ui::DrawablePtr(icon), 16.0f, 16.0f);
        glyph->KeepAspect = true;
        auto toggle = MakeRef<ui::ToggleButton>(MemoryAllocator());
        toggle->SetContent(RefPtr<ui::View>(glyph.Get()));
        toggle->SetStyle(ui::StyleProperty::Padding, ui::Thickness{4.0f, 3.0f});
        toggle->TooltipText = String(tooltip);
        // A radio pair, by the checked state rather than the click: a toggle flips on a click
        // and on Space alike, and a click on the checked one must not leave neither checked.
        ViewModeToggles* self = this;
        ui::ToggleButton* raw = toggle.Get();
        toggle->IsChecked.Changed.Add(ui::Event<void(bool)>::Handler{
            [self, raw, grid](bool checked)
            {
                if (checked && grid != self->m_grid)
                {
                    self->SetGridMode(grid);
                    if (self->OnModeChanged)
                    {
                        self->OnModeChanged(grid);
                    }
                }
                else if (!checked && grid == self->m_grid)
                {
                    // Back to checked. Silently: a property ignores a SetValue made from its own
                    // Changed handler, and the owner is invalidated right after this returns.
                    raw->IsChecked.SetSilent(true);
                }
            }});
        return toggle;
    }

    RTTI_DEFINE_OBJECT(ViewModeToggles, "rtti::editor::app")
}
