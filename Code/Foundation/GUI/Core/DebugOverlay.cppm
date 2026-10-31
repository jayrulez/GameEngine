// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :debug_overlay partition
//
// DebugOverlay: what a root draws over its tree to show how it works. For layout, it can show each
// node's border box, padding, margin and baseline. For input, it can show the focus path, the tab
// order, the candidates of the last directional move, the node under the pointer, and a flash on
// the nodes that asked for a redraw.

module;
#include "Core/Prelude.h"

export module foundation.gui:debug_overlay;


export namespace foundation::gui
{
    /// What a root's debug overlay shows (nothing by default).
    struct DebugOverlay
    {
        // Layout options, for working on a layout.
        bool Bounds = false;    ///< each node's border box (red)
        bool Padding = false;   ///< each node's padding (green)
        bool Margin = false;    ///< each node's margin (orange)
        bool Baselines = false; ///< the baseline of each node that has its own (magenta)
        // Input options.
        bool FocusPath = false;   ///< the focused node and each ancestor holding it, with their names
        bool TabOrder = false;    ///< the tab stops in order, numbered and joined
        bool Navigation = false;  ///< the last directional move: each candidate with its score or why it was rejected, and the one chosen
        bool Pointer = false;     ///< the node under the pointer, with its name and size
        bool RedrawFlash = false; ///< the nodes that invalidated themselves, fading after the frame

        [[nodiscard]] static constexpr DebugOverlay Layout() noexcept { return {true, true, true, true}; }
        [[nodiscard]] static constexpr DebugOverlay Input() noexcept
        {
            DebugOverlay o;
            o.FocusPath = o.TabOrder = o.Navigation = o.Pointer = o.RedrawFlash = true;
            return o;
        }
        [[nodiscard]] constexpr bool AnyLayout() const noexcept { return Bounds || Padding || Margin || Baselines; }
        [[nodiscard]] constexpr bool AnyInput() const noexcept { return FocusPath || TabOrder || Navigation || Pointer || RedrawFlash; }
        [[nodiscard]] constexpr bool Any() const noexcept { return AnyLayout() || AnyInput(); }
        [[nodiscard]] constexpr bool operator==(const DebugOverlay&) const noexcept = default;
    };
}
