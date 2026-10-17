// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :popup_placement partition
//
// Where a popup goes (E5 of Specs/gui-lab-evolution.md): one helper every popup places itself with,
// a combo box's list, a menu and its submenus, a tooltip and a context menu. A size goes against an
// anchor rectangle on a preferred side (below, above, right or left), aligned along that side
// (start, centre or end) and a gap away; when it does not fit there it flips to the other side if
// that side has more room, shrinks to the room on the side it goes (a list then scrolls), and is
// clamped into the bounds (the root's), all in the root's units.
// After Godot's popup clamping, GPUI's `anchored` and foundation.ui's PopupPositioner.

module;
#include "Core/Prelude.h"

export module experimental.gui:popup_placement;

import foundation.core; // Float2, Max, Min
import :rect;

using namespace foundation::core;
namespace core = foundation::core;

export namespace experimental::gui
{
    /// The side of the anchor a popup goes on.
    enum class PopupSide : u8
    {
        Below,
        Above,
        Right,
        Left,
    };
    /// How a popup lines up along the side it goes on: its start with the anchor's start (a
    /// dropdown's left edges, a submenu's tops), centred on it, or its end with the anchor's end.
    enum class PopupAlign : u8
    {
        Start,
        Center,
        End,
    };

    struct PopupPlacement
    {
        PopupSide Side = PopupSide::Below;
        PopupAlign Align = PopupAlign::Start;
        f32 Gap = 0.0f;      ///< between the anchor and the popup
        bool Shrink = true;  ///< made smaller to fit the room on its side, rather than covering the anchor
    };

    /// Where it went, and on which side (after a flip, the other one).
    struct PlacedPopup
    {
        core::Float2 Position{0.0f, 0.0f};
        core::Float2 Size{0.0f, 0.0f}; ///< what it was given, its own or shrunk to fit
        PopupSide Side = PopupSide::Below;
    };

    [[nodiscard]] constexpr PopupSide OppositeSide(PopupSide side) noexcept
    {
        switch (side)
        {
        case PopupSide::Below: return PopupSide::Above;
        case PopupSide::Above: return PopupSide::Below;
        case PopupSide::Right: return PopupSide::Left;
        default: return PopupSide::Right;
        }
    }

    namespace detail
    {
        // The room between the anchor and the bounds' edge on `side`.
        [[nodiscard]] constexpr f32 RoomOn(PopupSide side, const Rect& anchor, const Rect& bounds, f32 gap) noexcept
        {
            switch (side)
            {
            case PopupSide::Below: return bounds.y + bounds.height - (anchor.y + anchor.height) - gap;
            case PopupSide::Above: return anchor.y - bounds.y - gap;
            case PopupSide::Right: return bounds.x + bounds.width - (anchor.x + anchor.width) - gap;
            default: return anchor.x - bounds.x - gap;
            }
        }
        // Lined up along a side: from `start` (the anchor's) over `length`, a popup `extent` long.
        [[nodiscard]] constexpr f32 Aligned(PopupAlign align, f32 start, f32 length, f32 extent) noexcept
        {
            switch (align)
            {
            case PopupAlign::Center: return start + (length - extent) * 0.5f;
            case PopupAlign::End: return start + length - extent;
            default: return start;
            }
        }
        // Into [low, low + room]: a popup longer than the room starts at its low edge.
        [[nodiscard]] constexpr f32 Clamped(f32 at, f32 extent, f32 low, f32 room) noexcept
        {
            if (at + extent > low + room)
                at = low + room - extent;
            return at < low ? low : at;
        }
    }

    /// Places `size` against `anchor` within `bounds` as `placement` prefers.
    [[nodiscard]] constexpr PlacedPopup PlacePopup(const Rect& anchor, core::Float2 size, const Rect& bounds,
                                                   const PopupPlacement& placement = {}) noexcept
    {
        PopupSide side = placement.Side;
        const bool vertical = side == PopupSide::Below || side == PopupSide::Above;
        const f32 need = vertical ? size.y : size.x;
        const f32 room = detail::RoomOn(side, anchor, bounds, placement.Gap);
        if (room < need && detail::RoomOn(OppositeSide(side), anchor, bounds, placement.Gap) > room)
            side = OppositeSide(side);

        PlacedPopup out;
        out.Side = side;
        // Shrunk to the room on its side (along it) and to the bounds (across).
        if (placement.Shrink)
        {
            const f32 sideRoom = core::Max(0.0f, detail::RoomOn(side, anchor, bounds, placement.Gap));
            if (vertical)
            {
                size.y = core::Min(size.y, sideRoom);
                size.x = core::Min(size.x, bounds.width);
            }
            else
            {
                size.x = core::Min(size.x, sideRoom);
                size.y = core::Min(size.y, bounds.height);
            }
        }
        out.Size = size;
        const f32 gap = placement.Gap;
        switch (side)
        {
        case PopupSide::Below: out.Position.y = anchor.y + anchor.height + gap; break;
        case PopupSide::Above: out.Position.y = anchor.y - gap - size.y; break;
        case PopupSide::Right: out.Position.x = anchor.x + anchor.width + gap; break;
        case PopupSide::Left: out.Position.x = anchor.x - gap - size.x; break;
        }
        if (vertical)
            out.Position.x = detail::Aligned(placement.Align, anchor.x, anchor.width, size.x);
        else
            out.Position.y = detail::Aligned(placement.Align, anchor.y, anchor.height, size.y);
        out.Position.x = detail::Clamped(out.Position.x, size.x, bounds.x, bounds.width);
        out.Position.y = detail::Clamped(out.Position.y, size.y, bounds.y, bounds.height);
        return out;
    }

    /// A popup at a point (a context menu at the pointer): below and to the right of it, above it
    /// where it does not fit below, slid back in from the right edge.
    [[nodiscard]] constexpr PlacedPopup PlaceAtPoint(core::Float2 point, core::Float2 size, const Rect& bounds) noexcept
    {
        return PlacePopup(Rect{point.x, point.y, 0.0f, 0.0f}, size, bounds, PopupPlacement{});
    }
}
