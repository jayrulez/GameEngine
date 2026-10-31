// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// HelloWindow - the minimal app: opens a window and runs the frame loop
// until the window is closed. Demonstrates the full path from Core through
// Runtime (Context/Subsystem) and Shell (SDL3) to an ApplicationHost driving an
// IApplication, wired by APP_MAIN. Run it directly; close the window to exit.

#include "Core/Prelude.h"
#include "Runtime.Client/AppMain.h"

import foundation.core;
import foundation.runtime;
import foundation.runtime.client;
import foundation.shell;
import foundation.runtime.desktop;
import foundation.shell.desktop;
import foundation.graphics; // GraphicsDevice and FrameContext (APP_MAIN sets up the device)
import foundation.graphics.gpu; // CreateGraphicsDevice

namespace core = foundation::core;
namespace runtime = foundation::runtime;
namespace graphics = foundation::graphics;

namespace
{
    class HelloApp final : public runtime::IApplication
    {
    public:
        void OnStartup(runtime::IApplicationHost&) override
        {
            core::ConsoleWrite(u8"HelloWindow: started - close the window to exit.\n");
        }

        void OnUpdate(runtime::IApplicationHost&, core::f32 deltaTime) override
        {
            m_elapsed += deltaTime;
            ++m_frames;
        }

        void OnRenderWindow(runtime::IApplicationHost&, graphics::FrameContext& frame) override
        {
            frame.Clear(0.10f, 0.10f, 0.12f, 1.0f); // a calm dark grey
        }

        void OnShutdown(runtime::IApplicationHost&) override
        {
            core::ConsoleWrite(u8"HelloWindow: shutting down.\n");
        }

    private:
        core::f32 m_elapsed = 0.0f;
        core::u64 m_frames = 0;
    };
}

APP_MAIN(HelloApp)
