// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// MultiWindow - the runtime-host smoke test (not an RHI sample). It exercises the render host end
// to end: a shared GraphicsDevice, the Application's per-window render loop, and runtime window
// creation. It opens two OS windows, the main one and a second opened at runtime through
// OpenWindow(), and clears each to a different color every frame. Close the main window to exit.
//
// The path: CreateShell (SDL3), then CreateGraphicsDevice (Vulkan), then Application (with
// OpenWindow), then RunApplication. The app has no swapchain or loop code of its own.

#include "Core/Prelude.h"

import foundation.core;
import foundation.rhi;
import foundation.runtime;
import foundation.runtime.client;
import foundation.shell;
import foundation.runtime.desktop;
import foundation.shell.desktop;
import foundation.graphics;
import foundation.graphics.gpu;

namespace core = foundation::core;
namespace runtime = foundation::runtime;
namespace graphics = foundation::graphics;
namespace shell = foundation::shell;
namespace rhi = foundation::rhi;

namespace
{
    class MultiWindowApp final : public runtime::IApplication
    {
    public:
        void OnStartup(runtime::IApplicationHost& host) override
        {
            // windows[0] (the main window) already has a RenderWindow from Start().
            // Open a second OS window at runtime: the same call a detachable UI
            // panel would make.
            shell::WindowSettings ws;
            ws.title = u8"MultiWindow - Detached";
            ws.width = 480;
            ws.height = 360;
            m_second = host.OpenWindow(ws, graphics::RenderWindowDesc{});
            core::ConsoleWrite(u8"MultiWindow: two windows up - close the main window to exit.\n");
        }

        void OnRenderWindow(runtime::IApplicationHost&, graphics::FrameContext& frame) override
        {
            // Each window clears to its own color, proving independent per-window
            // presentation through the shared device.
            const rhi::ClearColor color = (frame.window == m_second)
                                              ? rhi::ClearColor{0.85f, 0.45f, 0.20f, 1.0f} // warm
                                              : rhi::ClearColor::CornflowerBlue();         // main
            frame.BeginBackbufferPass(color);
            frame.EndBackbufferPass();
        }

        void OnShutdown(runtime::IApplicationHost&) override
        {
            core::ConsoleWrite(u8"MultiWindow: shutting down.\n");
        }

    private:
        graphics::RenderWindow* m_second = nullptr;
    };
}

int main(int /*argc*/, char** /*argv*/)
{
    shell::WindowSettings ws;
    ws.title = u8"MultiWindow - Main";
    ws.width = 800;
    ws.height = 600;

    auto shell = shell::CreateShell(foundation::core::DefaultAllocator(), ws);
    if (shell.Get() == nullptr || shell->MainWindow() == nullptr)
    {
        core::ConsoleWrite(u8"MultiWindow: shell/window init failed.\n");
        return 1;
    }

    graphics::GraphicsDeviceDesc gdd;
    gdd.backend = graphics::BackendType::Vulkan;
    gdd.enableValidation = true;
    auto gpu = graphics::CreateGraphicsDevice(gdd);
    if (!gpu.HasValue())
    {
        core::ConsoleWrite(u8"MultiWindow: graphics device creation failed.\n");
        return 1;
    }

    MultiWindowApp app;
    return runtime::RunApplication(app, *shell, gpu.Value().Get());
}
