// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// An application's profiler overlay (Specs/profiler.md, P4; on the GUI, Specs/engine-ui-removal.md,
// layer 1): off until the app shows it; shown, it sits on the GUI's overlay layer taking no
// pointer (so the layer stays passive), arms the profiler and reads its frames; hidden, it leaves
// the layer and lets the profiler go.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.vfs;
import foundation.runtime;
import foundation.scene;
import engine.scene;
import foundation.profiler;
import foundation.profiler.gui;
import engine.gui;
import engine.defaultapp;

using namespace foundation::core;
namespace runtime = foundation::runtime;
namespace scene = foundation::scene;
using engine::gui::GuiSubsystem;

namespace
{
    foundation::vfs::IFileSystem& DataFs()
    {
        static foundation::vfs::NativeFileSystem fs(foundation::vfs::FindDataRoot(), DefaultAllocator());
        return fs;
    }
}

TEST_CASE("profiler overlay host: off until shown, then on the overlay layer, armed and reading")
{
    runtime::SubsystemManager ctx(DefaultAllocator());
    auto* scenes = ctx.AddSubsystem<engine::scene::SceneSubsystem>();
    scene::SceneManager sm{DefaultAllocator()};
    scenes->RegisterManager(&sm);
    auto* gui = ctx.AddSubsystem<GuiSubsystem>(DefaultAllocator(), DataFs());
    ctx.Startup();

    foundation::profiler::Profiler profiler(DefaultAllocator());
    engine::runtime::ProfilerOverlayHost host(DefaultAllocator(), profiler);
    const usize before = gui->OverlayCount();
    CHECK_FALSE(host.Shown());
    CHECK(host.Overlay() == nullptr);
    CHECK_FALSE(profiler.Armed());

#if BUILD_PROFILING
    host.Toggle(*gui);
    REQUIRE(host.Shown());
    REQUIRE(host.Overlay() != nullptr);
    CHECK(gui->OverlayCount() == before + 1);
    CHECK(host.Overlay()->GetParent() == gui->OverlayLayer());
    CHECK_FALSE(host.Overlay()->IsHitTestVisible()); // the layer stays passive: the game keeps the pointer
    CHECK(profiler.Armed());

    // A frame recorded while shown reaches its views at the next refresh.
    profiler.BeginFrame();
    REQUIRE(profiler.BeginScope("Update"));
    profiler.EndScope();
    profiler.EndFrame();
    host.Refresh();
    REQUIRE(host.Overlay()->Scopes().Size() == 1);
    CHECK(host.Overlay()->Scopes()[0].name == StringView(u8"Update"));

    host.Toggle(*gui);
    CHECK_FALSE(host.Shown());
    CHECK(gui->OverlayCount() == before);
    CHECK_FALSE(profiler.Armed());

    host.Show(*gui); // shown again: the same overlay
    host.Show(*gui); // once only
    CHECK(gui->OverlayCount() == before + 1);
    host.Hide();
#else
    host.Show(*gui); // compiled out: nothing shows
    CHECK_FALSE(host.Shown());
#endif
    CHECK_FALSE(profiler.Armed());
    ctx.Shutdown();
}
