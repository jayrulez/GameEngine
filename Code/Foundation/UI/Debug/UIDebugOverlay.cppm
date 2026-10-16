// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - :ui_debug_overlay partition
//
// Draws debug overlays (bounds / padding / margin / hit-target / focus) for a view, in the view's local
// space. Called after the normal draw pass in ViewGroup's DrawChildren. Ported from Sedulous.UI/src/Debug/
// UIDebugOverlay.bf (a `static class` -> a struct of static methods). View is only forward-declared here
// (the body, in Debug/UIDebugOverlayImpl.cpp, reaches into the full View/ViewGroup cluster) so :view can
// import this partition to call it without a module cycle.

module;
#include "Core/Prelude.h"

export module foundation.ui:ui_debug_overlay;

import foundation.core;
import :draw_context; // UIDrawContext (referenced in the signature; body uses ctx.VG()/DebugSettings())

export namespace foundation::ui
{
    class View;

    struct UIDebugOverlay
    {
        /// Draw debug overlays for a single view (called in the view's local coordinate space);
        /// `drawIndex` is its place in its parent's draw order (ShowZOrder).
        static void DrawOverlays(UIDrawContext& ctx, View& view, usize drawIndex);
    };
}
