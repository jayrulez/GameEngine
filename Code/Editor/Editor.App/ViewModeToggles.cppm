// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell
// Editor App - :view_mode_toggles partition
//
// ViewModeToggles: the List / Grid pair over a view of assets (the asset browser, the asset
// picker) - two icon toggles, one checked, with the words as their tooltips. The owner shows the
// list or the grid; the pair only says which and reports a click.
module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module editor.app:view_mode_toggles;

import foundation.core;
import foundation.ui;

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

        [[nodiscard]] ui::ToggleButton* ListToggle() const noexcept { return m_list.Get(); }
        [[nodiscard]] ui::ToggleButton* GridToggle() const noexcept { return m_gridToggle.Get(); }

    private:
        [[nodiscard]] RefPtr<ui::ToggleButton> MakeToggle(ui::SVGDrawable* icon, StringView tooltip,
                                                          bool grid);

        RefPtr<ui::ToggleButton> m_list;
        RefPtr<ui::ToggleButton> m_gridToggle;
        bool m_grid = false;
    };
}
