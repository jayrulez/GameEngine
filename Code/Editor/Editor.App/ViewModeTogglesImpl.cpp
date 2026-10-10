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
        m_segments = MakeRef<SegmentedToggle>(MemoryAllocator());
        m_segments->Spacing = 2.0f;
        ViewModeToggles* self = this;
        m_segments->Build(
            2,
            [self](i32 index)
            {
                // Null drawables before EditorIcons::Initialize (tests).
                EditorIcons& icons = EditorIcons::Get();
                ui::SVGDrawable* icon = index == 0 ? icons.viewList.Get() : icons.viewGrid.Get();
                auto glyph = MakeRef<ui::DrawableView>(self->MemoryAllocator(), ui::DrawablePtr(icon),
                                                       16.0f, 16.0f);
                glyph->KeepAspect = true;
                return RefPtr<ui::View>(glyph.Get());
            },
            [self](i32 index)
            {
                const bool grid = index == 1;
                if (grid != self->m_grid)
                {
                    self->m_grid = grid;
                    if (self->OnModeChanged)
                    {
                        self->OnModeChanged(grid);
                    }
                }
            },
            [self]() { return self->m_grid ? 1 : 0; },
            [](i32 index) { return index == 0 ? StringView(u8"List") : StringView(u8"Grid"); });
        for (usize k = 0; k < m_segments->ChildCount(); ++k)
        {
            if (ui::ToggleButton* toggle = Segment(k))
            {
                toggle->SetStyle(ui::StyleProperty::Padding, ui::Thickness{4.0f, 3.0f});
            }
        }
        ui::LayoutStyle center;
        center.AlignSelf = ui::Align::Center;
        AddView(m_segments.Get(), center);
    }

    void ViewModeToggles::SetGridMode(bool grid)
    {
        m_grid = grid;
        m_segments->Refresh();
    }

    ui::ToggleButton* ViewModeToggles::Segment(usize index) const noexcept
    {
        return index < m_segments->ChildCount() ? Cast<ui::ToggleButton>(m_segments->GetChildAt(index))
                                                : nullptr;
    }

    RTTI_DEFINE_OBJECT(ViewModeToggles, "rtti::editor::app")
}
