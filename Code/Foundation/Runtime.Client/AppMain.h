// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// APP_MAIN(AppType) - generates the program entry point for a client app.
//
// This is a classic header (macros can't live in a module). Use it in the app's
// main translation unit, which must import:
//   - foundation.runtime.client          (IApplication)
//   - foundation.runtime.desktop         (RunApplication, the desktop runner)
//   - foundation.shell.desktop           (CreateShell)
//   - foundation.graphics + foundation.graphics.gpu (the GPU device)
// The entry point creates the shell window, a GraphicsDevice (so the window
// actually presents; without one the window never becomes visible on Wayland),
// the app, and hands all three to the desktop runner (which drives an
// ApplicationHost). AppType must be an IApplication (or DefaultApplication), and
// should clear/draw in OnRenderWindow (e.g. `frame.Clear(...)`).
//
//   import foundation.runtime.client;
//   import foundation.runtime.desktop;
//   import foundation.shell.desktop;
//   import foundation.graphics;
//   import foundation.graphics.gpu;
//   #include "Runtime.Client/AppMain.h"
//   class MyApp final : public foundation::runtime::IApplication { ... };
//   APP_MAIN(MyApp)

#ifndef FOUNDATION_RUNTIME_CLIENT_APPMAIN_H
#define FOUNDATION_RUNTIME_CLIENT_APPMAIN_H

#include "Core/Prelude.h" // PLATFORM_WEB (picks the desktop vs browser entry body)
#include "Core/Log/Log.h"  // the build-stamp startup line

// The build identity compiled into Runtime.Client (GenerateBuildStamp.cmake): git short
// hash, dirty flag and build minute. Logged first thing by APP_MAIN so a running
// binary, especially a browser-cached .wasm, can always be matched to a build.
extern "C" const char* BuildStamp();

#if PLATFORM_WEB

// Emscripten entry. Unlike desktop, the web runner (foundation.runtime.web) does not block: it
// registers a requestAnimationFrame callback and returns, and the browser drives frames after
// main() unwinds. So the shell, the GPU device and the app must outlive main's return; they live
// in static storage here. The backend is always WebGPU (there is no argv), and device creation
// blocks and yields to the browser via ASYNCIFY (see the WebGPU backend's pump), so the link must
// enable ASYNCIFY. The app TU imports foundation.shell.web and foundation.runtime.web (not the
// desktop pair). The canvas defaults to "#canvas" (Emscripten's default shell canvas id).
#define APP_MAIN(AppType)                                                                 \
    int main()                                                                                     \
    {                                                                                              \
        static ::foundation::core::ConsoleSink appConsoleSink;                                  \
        ::foundation::core::GlobalLogger().AddSink(&appConsoleSink);                            \
        ::foundation::core::GlobalLogger().SetMinLevel(::foundation::core::LogLevel::Info);            \
        LOG_INFO(u8"Build", u8"Client build {}",                                        \
                          reinterpret_cast<const char8_t*>(BuildStamp()));                 \
        static ::foundation::shell::WebShell appShell;                                          \
        ::foundation::graphics::GraphicsDeviceDesc appGpuDesc{};                                \
        appGpuDesc.backend = ::foundation::graphics::BackendType::WebGPU;                       \
        static auto appGpu = ::foundation::graphics::CreateGraphicsDevice(appGpuDesc);     \
        ::foundation::graphics::GraphicsDevice* appDevice =                                     \
            appGpu.HasValue() ? appGpu.Value().Get() : nullptr;                          \
        static AppType appInstance;                                                                \
        return ::foundation::runtime::RunApplication(appInstance, appShell, appDevice);    \
    }

#else

// The body is the same on all desktop OSes. A windowed Win32 build wants wWinMain
// (no console); main is correct for console/CI builds. Android targets provide
// their own entry.
// If GPU device creation fails (no Vulkan), the app still runs headless, without a window.
#define APP_MAIN(AppType)                                                                 \
    int main(int argc, char** argv)                                                                \
    {                                                                                              \
        static ::foundation::core::ConsoleSink appConsoleSink;                                  \
        ::foundation::core::GlobalLogger().AddSink(&appConsoleSink);                            \
        LOG_INFO(u8"Build", u8"Client build {}",                                        \
                          reinterpret_cast<const char8_t*>(BuildStamp()));                 \
        auto shell =                                                                               \
            ::foundation::shell::CreateShell(::foundation::core::DefaultAllocator());                                             \
        ::foundation::graphics::GraphicsDeviceDesc appGpuDesc =                                 \
            ::foundation::graphics::DeviceDescFromArguments(argc, argv);                             \
        auto appGpu = ::foundation::graphics::CreateGraphicsDevice(appGpuDesc);            \
        ::foundation::graphics::GraphicsDevice* appDevice =                                     \
            appGpu.HasValue() ? appGpu.Value().Get() : nullptr;                          \
        AppType app;                                                                               \
        app.OnCommandLine(argc, argv);                                                             \
        return ::foundation::runtime::RunApplication(app, *shell, appDevice);                   \
    }

#endif // PLATFORM_WEB

#endif // FOUNDATION_RUNTIME_CLIENT_APPMAIN_H
