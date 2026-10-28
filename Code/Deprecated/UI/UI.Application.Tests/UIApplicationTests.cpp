// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI.Application tests: the dockable-window chrome policy.
// RuntimeDockableWindowHost itself needs a live GraphicsDevice + UIHost, so the unit under test
// here is the pure platform-policy mapping the host resolves through; the host-side wiring
// (HasOSChrome propagation, close routing, adorner behavior) is pinned headless in
// UI.Toolkit.Tests/DockingTests.cpp against a fake IDockableWindowHost.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import foundation.shell;
import foundation.ui.application;

using namespace foundation::ui::application;
namespace shell = foundation::shell;

// Linux window systems (X11/Wayland) default to OS-chromed floats - Wayland punishes
// app-positioned borderless windows, XWayland blocks cross-monitor drags, and the ruling is one
// platform default with NO true-X11 special case. Everything else keeps borderless floats.
TEST_CASE("chrome policy: Linux window systems prefer OS-chromed dockables")
{
    CHECK(PrefersOSChromedDockables(shell::WindowSystem::X11));
    CHECK(PrefersOSChromedDockables(shell::WindowSystem::Wayland));

    CHECK(!PrefersOSChromedDockables(shell::WindowSystem::Win32));
    CHECK(!PrefersOSChromedDockables(shell::WindowSystem::Cocoa));
    CHECK(!PrefersOSChromedDockables(shell::WindowSystem::Web));
    CHECK(!PrefersOSChromedDockables(shell::WindowSystem::Unknown));
}
