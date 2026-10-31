// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI.Application tests: the dockable-window chrome policy.
// RuntimeDockableWindowHost needs a live GraphicsDevice and UIHost, so these tests cover the pure
// platform-policy mapping the host resolves through. The host-side wiring (HasOSChrome
// propagation, close routing, adorner behavior) is tested headless in
// UI.Toolkit.Tests/DockingTests.cpp against a fake IDockableWindowHost.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import foundation.shell;
import foundation.ui.application;

using namespace foundation::ui::application;
namespace shell = foundation::shell;

// Linux window systems (X11 and Wayland) default to OS-chromed floats: Wayland handles
// app-positioned borderless windows badly and XWayland blocks cross-monitor drags. There is one
// platform default, with no special case for native X11. Everything else keeps borderless floats.
TEST_CASE("chrome policy: Linux window systems prefer OS-chromed dockables")
{
    CHECK(PrefersOSChromedDockables(shell::WindowSystem::X11));
    CHECK(PrefersOSChromedDockables(shell::WindowSystem::Wayland));

    CHECK(!PrefersOSChromedDockables(shell::WindowSystem::Win32));
    CHECK(!PrefersOSChromedDockables(shell::WindowSystem::Cocoa));
    CHECK(!PrefersOSChromedDockables(shell::WindowSystem::Web));
    CHECK(!PrefersOSChromedDockables(shell::WindowSystem::Unknown));
}
