// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :frame_stats partition
//
// FrameStats: what a root's last frame cost and why it was needed. Each phase writes its part as it
// runs. The style pass records its time and the widgets it restyled; the layout pass records its
// time and the containers it laid out; the draw records its time, the nodes it drew and culled, and
// the VG geometry they made. NeedsFrame records the reasons it found, so a GUI that should be idle
// but is not says what keeps it awake. A host's frame stats, the inspector's frames tab and tests
// read it.

module;
#include "Core/Prelude.h"

export module foundation.gui:frame_stats;

import foundation.core;

using namespace foundation::core;
namespace core = foundation::core;

export namespace foundation::gui
{
    /// Why a root asked for a frame, as NeedsFrame last found. Each flag is true when that reason
    /// applied.
    struct FrameReasons
    {
        bool Redraw = false;        ///< something shown was invalidated
        bool Layout = false;        ///< a layout is pending
        bool Pending = false;       ///< a binding or a queued mutation is pending
        bool HeldDirection = false; ///< a held direction repeats
        bool Animation = false;     ///< an action runs on a shown node (AnimatedNode names the first)
        bool Overlay = false;       ///< the debug overlay's redraw flashes are fading

        [[nodiscard]] bool Any() const noexcept { return Redraw || Layout || Pending || HeldDirection || Animation || Overlay; }
    };

    struct FrameStats
    {
        FrameReasons Reasons;
        core::String AnimatedNode; ///< the first shown node an action runs on, when Reasons.Animation is set

        f64 StyleMs = 0.0;
        u32 Restyled = 0; ///< widgets the style pass restyled
        f64 LayoutMs = 0.0;
        u32 LaidOut = 0;  ///< containers the layout pass laid out
        f64 DrawMs = 0.0;
        u32 NodesDrawn = 0;
        u32 NodesCulled = 0; ///< nodes skipped as outside the clip
        u32 Vertices = 0;    ///< the VG geometry the draw made
        u32 Commands = 0;
        u64 FramesDrawn = 0; ///< how many frames this root has drawn through DrawFrame

        /// Describes the frame in one line: the phases' times and counts, then the reasons.
        [[nodiscard]] core::String Describe() const
        {
            const auto us = [](f64 ms) { return static_cast<u64>(ms * 1000.0 + 0.5); }; // microseconds read better than ms here
            core::String text = core::Format(u8"style {} us ({} restyled), layout {} us ({} laid out), draw {} us ({} drawn, {} culled, {} vertices, {} commands)",
                                             us(StyleMs), Restyled, us(LayoutMs), LaidOut, us(DrawMs), NodesDrawn, NodesCulled, Vertices, Commands);
            text += core::StringView(u8"; asked by:");
            if (!Reasons.Any())
                text += core::StringView(u8" nothing");
            if (Reasons.Redraw)
                text += core::StringView(u8" redraw");
            if (Reasons.Layout)
                text += core::StringView(u8" layout");
            if (Reasons.Pending)
                text += core::StringView(u8" pending");
            if (Reasons.HeldDirection)
                text += core::StringView(u8" held-direction");
            if (Reasons.Animation)
            {
                text += core::StringView(u8" animation (");
                text += AnimatedNode.AsView();
                text += core::StringView(u8")");
            }
            if (Reasons.Overlay)
                text += core::StringView(u8" overlay");
            return text;
        }
    };
}
