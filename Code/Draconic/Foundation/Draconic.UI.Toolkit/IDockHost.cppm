// Draconic UI Toolkit - :idock_host partition
//
// Interface for the docking system host that manages floating panels. Implemented by DockManager.
// Ported from Sedulous.UI.Toolkit/src/Docking/IDockHost.bf. Pure-abstract; DockablePanel/DockableWindow
// are only forward-declared (used behind pointers). Beef `UIContext Context { get; }` is renamed to
// `HostContext()` here to avoid colliding with View's public `Context` data member (DockManager inherits
// both View and IDockHost; a member function `Context()` cannot coexist with the inherited field).

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.ui.toolkit:idock_host;

import draconic.foundation;
import draconic.ui;

using namespace draconic::foundation;

export namespace draconic::ui::toolkit
{
    class DockablePanel;
    class DockableWindow;

    /// Interface for the docking system host that manages floating panels.
    class IDockHost
    {
    public:
        virtual ~IDockHost() = default;

        virtual void FloatPanel(DockablePanel* panel, f32 x, f32 y) = 0;
        virtual void DestroyDockableWindow(DockableWindow* fw) = 0;

        /// The UIContext of the host (Beef `Context` property; renamed to avoid the View::Context field clash).
        [[nodiscard]] virtual UIContext* HostContext() = 0;
    };
}
