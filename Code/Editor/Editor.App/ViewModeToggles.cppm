// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell
// Editor App - :view_mode_toggles partition
//
// ViewModeToggles: the List / Grid pair over a view of assets (the asset browser, the asset
// picker) - two icon toggles, one checked, with the words as their tooltips: a SegmentedToggle of
// two, the panels' exclusive row. The owner shows the list or the grid; the pair only says which
// and reports a choice.
module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module editor.app:view_mode_toggles;

import foundation.core;
import foundation.ui;
import :tool_panel_widgets; // SegmentedToggle

using namespace foundation::core;
namespace ui = foundation::ui;

export namespace editor::app
{
    class ViewModeToggles final : public ui::FlexLayout
    {
        RTTI_OBJECT(ViewModeToggles, ui::FlexLayout)
    public:
        /// Called with the mode a click asked for (true = grid). Not called by SetGridMode.
        Function<void(bool)> OnModeChanged;

        ViewModeToggles();

        /// Check the toggle for `grid` and uncheck the other (no callback).
        void SetGridMode(bool grid);
        [[nodiscard]] bool GridMode() const noexcept { return m_grid; }

        [[nodiscard]] ui::ToggleButton* ListToggle() const noexcept { return Segment(0); }
        [[nodiscard]] ui::ToggleButton* GridToggle() const noexcept { return Segment(1); }

    private:
        [[nodiscard]] ui::ToggleButton* Segment(usize index) const noexcept;

        RefPtr<SegmentedToggle> m_segments;
        bool m_grid = false;
    };
}
