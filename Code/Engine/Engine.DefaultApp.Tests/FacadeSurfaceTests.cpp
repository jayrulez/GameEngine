// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The RUNTIME-path facade-surface tripwire (sibling of StandardFactoriesTests, same incident
// class). Incident 2026-08-22: RegisterAllScriptFacades existed and was tested, but the runtime
// app kept a hand-rolled per-subsystem registration list that drifted past RegisterRunScriptFacade
// - game scripts compiled against a prelude with no `run`, masked in editor/PIE (the editor calls
// the root) and only visible in an exported game. Configure now calls the root; these tests pin
// that the RUNTIME path yields the complete surface, with `run` as the specific regression pin.

#include <doctest/doctest.h>

#include "Core/Prelude.h"

import foundation.core;
import foundation.runtime;
import foundation.runtime.client;
import foundation.shell;
import foundation.graphics;
import foundation.script.facades;
import engine.composition;
import engine.defaultapp;
import foundation.input; // IInputSourceProvider
import engine.input;  // FittedInputSource (the render resolution's pointer)
import engine.gui;    // the screen tier's resolution
import engine.gameinstance;

using namespace foundation::core;
namespace runtime = foundation::runtime;

namespace
{
    // Headless host stub (the StandardFactoriesTests shape): no shell, no graphics, no windows.
    // Configure guards on null Graphics()/Shell(), so the full subsystem set wires headless.
    class StubHost final : public runtime::IApplicationHost
    {
    public:
        runtime::Context& Ctx() noexcept override { return m_context; }
        foundation::shell::IShell* Shell() noexcept override { return nullptr; }
        foundation::graphics::GraphicsDevice* Graphics() noexcept override { return nullptr; }
        foundation::graphics::RenderWindow* MainRenderWindow() noexcept override
        {
            return nullptr;
        }
        foundation::graphics::RenderWindow*
        OpenWindow(const foundation::shell::WindowSettings&,
                   const foundation::graphics::RenderWindowDesc&) override
        {
            return nullptr;
        }
        void CloseWindow(foundation::graphics::RenderWindow*) override {}
        void RequestExit(int) override {}

    private:
        runtime::Context m_context{DefaultAllocator()};
    };

    [[nodiscard]] bool SurfaceHasName(StringView name)
    {
        for (StringView entry : foundation::script::ExtraFacadeNames())
        {
            if (entry == name)
            {
                return true;
            }
        }
        return false;
    }
}

TEST_CASE("defaultapp: Configure registers the COMPLETE facade surface (the run pin)")
{
    StubHost host;
    engine::runtime::DefaultApplication app;
    app.Configure(host);

    // The count tripwire, now measured on the RUNTIME path: Configure must yield exactly the
    // surface root's set (a facade registered anywhere else without the root would diverge here).
    CHECK(foundation::script::ExtraFacadeNames().Size() == engine::kSubsystemFacadeNameCount);

    // The specific regression pin: the exported game's `run::loadScene` gap.
    CHECK(SurfaceHasName(u8"run"));
}

// Sedulous f9b1feb7: a render resolution fits the game into the window: its instances read the
// pointer through a fitted source (render pixels), and the screen UI lays out at that size.
// Nought goes back to the window's own size and the shell's devices.
TEST_CASE("defaultapp: a render resolution fits the game's pointer and its screen UI")
{
    StubHost host;
    engine::runtime::DefaultApplication app;
    app.Configure(host);
    REQUIRE(app.Input() != nullptr);
    REQUIRE(app.GUI() != nullptr);
    foundation::input::IInputSourceProvider* shell = &app.Input()->ShellSource();
    CHECK(app.Instance().InputSource() == shell);
    CHECK_FALSE(app.HasRenderResolution());

    app.SetRenderResolution(320, 180, FitMode::Letterbox);
    CHECK(app.HasRenderResolution());
    CHECK(app.GUI()->HasScreenResolution());
    foundation::input::IInputSourceProvider* fitted = app.Instance().InputSource();
    REQUIRE(fitted != nullptr);
    CHECK(fitted != shell);
    CHECK(&app.Input()->ActiveSource() == fitted);
    // An instance made after reads the fitted source too.
    engine::runtime::GameInstance* extra = app.CreateInstance();
    REQUIRE(extra != nullptr);
    CHECK(extra->InputSource() == fitted);

    app.SetRenderResolution(0, 0, FitMode::Letterbox);
    CHECK_FALSE(app.HasRenderResolution());
    CHECK_FALSE(app.GUI()->HasScreenResolution());
    CHECK(app.Instance().InputSource() == shell);
    CHECK(extra->InputSource() == shell);
    CHECK(&app.Input()->ActiveSource() == shell);
    app.ReleaseInstance(extra);
}
