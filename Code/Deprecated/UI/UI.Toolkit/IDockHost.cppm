// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI Toolkit - :idock_host partition
//
// Interface for the docking system host that manages floating panels. Implemented by DockManager.
// Pure abstract; DockablePanel and DockableWindow are only forward-declared (used behind pointers).
// The context accessor is named `HostContext()` because DockManager inherits both View and IDockHost,
// and a member function `Context()` cannot coexist with View's public `Context` data member.

module;
#include "Core/Prelude.h"

export module foundation.ui.toolkit:idock_host;

import foundation.core;
import foundation.ui;

using namespace foundation::core;

export namespace foundation::ui::toolkit
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

        /// The UIContext of the host (not `Context`, which would clash with the View::Context field).
        [[nodiscard]] virtual UIContext* HostContext() = 0;
    };
}
