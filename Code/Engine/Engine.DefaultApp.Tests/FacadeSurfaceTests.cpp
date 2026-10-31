// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The runtime-path facade-surface tripwire (a sibling of StandardFactoriesTests). The editor calls
// RegisterAllScriptFacades itself, so a facade the runtime app's Configure fails to register shows
// only in an exported game, whose scripts then compile against a prelude without it. These tests
// pin that the runtime path yields the complete surface, with `run` checked by name.

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
import foundation.run;

using namespace foundation::core;
namespace runtime = foundation::runtime;

namespace
{
    // Headless host stub (shaped like StandardFactoriesTests'): no shell, no graphics, no windows.
    // Configure guards on null Graphics()/Shell(), so the full subsystem set wires headless.
    class StubHost final : public runtime::IApplicationHost
    {
    public:
        runtime::SubsystemManager& Subsystems() noexcept override { return m_context; }
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
        runtime::SubsystemManager m_context{DefaultAllocator()};
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

    // The count tripwire, measured on the runtime path: Configure must yield exactly the surface
    // root's set (a facade registered anywhere else without the root would diverge here).
    CHECK(foundation::script::ExtraFacadeNames().Size() == engine::kSubsystemFacadeNameCount);

    // `run` by name: the facade an exported game's run.loadScene needs.
    CHECK(SurfaceHasName(u8"run"));
}

// A render resolution fits the game into the window: its instances read the
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

// The runs: the app's instances are its runs, listed by
// its run manager, which every subsystem finds on the subsystem manager; an instance released is
// announced gone and leaves; the focused one, when it goes, takes the focus with it.
TEST_CASE("defaultapp: its instances are the runs, on the subsystem manager for every subsystem")
{
    struct Heard final : foundation::run::IRunListener
    {
        int created = 0;
        int destroyed = 0;
        int focusCleared = 0;
        void OnRunCreated(foundation::run::IRun&) override { ++created; }
        void OnRunDestroyed(foundation::run::IRun&) override { ++destroyed; }
        void OnFocusChanged(foundation::run::IRun* run) override { focusCleared += run == nullptr ? 1 : 0; }
    } heard;

    StubHost host;
    engine::runtime::DefaultApplication app;
    app.Configure(host);
    foundation::run::IRunManager* runs = host.Subsystems().RunManager();
    REQUIRE(runs != nullptr);
    CHECK(runs == app.Runs());
    CHECK(runs->Find(app.Instance().Id()) == &app.Instance()); // the primary instance is a run

    runs->Subscribe(heard);
    engine::runtime::GameInstance* extra = app.CreateInstance();
    REQUIRE(extra != nullptr);
    CHECK(heard.created == 1);
    CHECK(extra->Name() == StringView(u8"Game 2"));
    const foundation::run::RunId id = extra->Id();
    CHECK(runs->Find(id) == extra);
    runs->SetFocused(extra);

    app.ReleaseInstance(extra);
    CHECK(heard.destroyed == 1);
    CHECK(heard.focusCleared == 1);
    CHECK(runs->Find(id) == nullptr);
    CHECK(runs->Focused() == nullptr);
    engine::runtime::GameInstance* next = app.CreateInstance();
    REQUIRE(next != nullptr);
    CHECK(next->Id() != id); // ids are not reused
    runs->Unsubscribe(heard);
    app.ReleaseInstance(next);
}

