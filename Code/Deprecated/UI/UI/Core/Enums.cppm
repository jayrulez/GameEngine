// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - :enums partition
//
// Small core enums (Visibility, Orientation, CursorType, InvalidationKind), grouped
// into one partition.

export module foundation.ui:enums;

export namespace foundation::ui
{
    /// View visibility state.
    enum class Visibility
    {
        /// Visible and participates in layout.
        Visible,
        /// Invisible but still occupies space in layout.
        Hidden,
        /// Invisible and does not participate in layout.
        Gone
    };

    enum class Orientation
    {
        Horizontal,
        Vertical
    };

    /// Cursor appearance when hovering a view. Views set Cursor to change the cursor on
    /// hover; EffectiveCursor walks the parent chain to the first non-Default value.
    enum class CursorType
    {
        Default,
        Arrow,
        Hand,
        IBeam,
        Crosshair,
        SizeNS,
        SizeWE,
        SizeNWSE,
        SizeNESW,
        Move,
        NotAllowed,
        Wait
    };

    /// What kind of invalidation a property change triggers.
    enum class InvalidationKind
    {
        /// Re-measure, re-layout and redraw. The default for all properties unless overridden.
        Layout,
        /// Redraw only, no layout recalculation (visual-only props: Opacity, TextColor, Cursor).
        Visual
    };
}
