// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - :itooltip_provider partition
//
// Implement on a View to provide custom tooltip content instead of plain text. TooltipManager asks
// for this interface first, through the virtual View::AsTooltipProvider(), and falls back to
// View.TooltipText when the view has none.

module;
#include "Core/Prelude.h"

export module foundation.ui:itooltip_provider;

import foundation.core;

using namespace foundation::core;

export namespace foundation::ui
{
    class View;

    class ITooltipProvider
    {
    public:
        virtual ~ITooltipProvider() = default;
        /// Create the tooltip content view. Ownership transfers to the TooltipView. Return null to
        /// suppress the tooltip.
        [[nodiscard]] virtual RefPtr<View> CreateTooltipContent() = 0;
    };
}
