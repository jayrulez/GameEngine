// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - :itooltip_provider partition
//
// Implement on a View to provide custom tooltip content instead of plain text. TooltipManager checks
// for this interface first (via View::AsTooltipProvider()); if absent, falls back to View.TooltipText.
// Ported from Sedulous.UI/src/Overlay/ITooltipProvider.bf. Pattern A (tree-queried) - the interface is
// a plain abstract base; a View exposes it via a virtual AsTooltipProvider() capability query. Beef
// `View CreateTooltipContent()` transfers ownership to the TooltipView -> returns RefPtr<View> (RAII).

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
