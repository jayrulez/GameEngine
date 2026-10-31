// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - :tooltip_placement partition
//
// Where a tooltip appears relative to its anchor view.

module;
#include "Core/Prelude.h"

export module foundation.ui:tooltip_placement;

export namespace foundation::ui
{
    /// Where the tooltip appears relative to the anchor view.
    enum class TooltipPlacement
    {
        /// Below the anchor; flips above if clipping bottom.
        Bottom,
        /// Above the anchor; flips below if clipping top.
        Top,
        /// Right of the anchor; flips left if clipping right.
        Right,
        /// Left of the anchor; flips right if clipping left.
        Left,
        /// At the mouse pointer (a small offset below and right, clamped to the screen). For large
        /// views with per-region tooltip content (a code editor's diagnostic lines), where
        /// anchoring to the whole view's bounds lands nowhere near the hovered spot.
        Pointer
    };
}
