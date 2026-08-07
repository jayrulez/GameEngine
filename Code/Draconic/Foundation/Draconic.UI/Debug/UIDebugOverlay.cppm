// Draconic UI - :ui_debug_overlay partition
//
// Draws debug overlays (bounds / padding / margin / hit-target / focus) for a view, in the view's local
// space. Called after the normal draw pass in ViewGroup's DrawChildren. Ported from Sedulous.UI/src/Debug/
// UIDebugOverlay.bf (a `static class` -> a struct of static methods). View is only forward-declared here
// (the body, in Debug/UIDebugOverlayImpl.cpp, reaches into the full View/ViewGroup cluster) so :view can
// import this partition to call it without a module cycle.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.ui:ui_debug_overlay;

import draconic.foundation;
import :draw_context; // UIDrawContext (referenced in the signature; body uses ctx.VG()/DebugSettings())

export namespace draconic::ui
{
    class View;

    struct UIDebugOverlay
    {
        /// Draw debug overlays for a single view (called in the view's local coordinate space).
        static void DrawOverlays(UIDrawContext& ctx, View& view);
    };
}
