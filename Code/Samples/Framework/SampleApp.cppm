// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// SampleApp - abstract base for RHI samples (adapted from a sample
// framework to this engine.s shell). Brings up a window (foundation.shell),
// a Vulkan backend (validation-wrapped), device, queue, and swap chain; pumps
// events, tracks timing, and calls OnRender(). Resize is detected by polling the
// window size; the loop skips rendering while minimized.

module;
#include "Core/Prelude.h"
#include <cstdio>
#include <cstring>

export module samples.framework:sample_app;

import foundation.core;
import foundation.rhi;
#ifdef OPTION_HAS_VULKAN
import foundation.rhi.vulkan;
#endif
#ifdef OPTION_HAS_DX12
import foundation.rhi.dx12;
#endif
#ifdef OPTION_HAS_WEBGPU
import foundation.rhi.webgpu;
#endif
import foundation.rhi.validation;
import foundation.shell;
import foundation.shell.desktop;
import foundation.vfs; // ResolveDataRoot + the NativeFileSystem mounted over the data root
import foundation.rhi.capture; // ScreenshotCapture + the --screenshot flags
import foundation.image;       // Image (the written screenshot)

using namespace foundation::core;
using namespace foundation;
namespace rhi = foundation::rhi;
namespace shell = foundation::shell;
namespace vfs = foundation::vfs;

export namespace samples::framework
{

    enum class BackendType
    {
        Vulkan,
        DX12,
        WebGPU
    };

    // The default backend is whichever is compiled in - Vulkan on Linux, DX12 on Windows, WebGPU as
    // a fallback - so a sample run with no --backend flag works on any platform.
#if defined(OPTION_HAS_VULKAN)
    inline constexpr BackendType kDefaultBackend = BackendType::Vulkan;
#elif defined(OPTION_HAS_DX12)
    inline constexpr BackendType kDefaultBackend = BackendType::DX12;
#else
    inline constexpr BackendType kDefaultBackend = BackendType::WebGPU;
#endif

    class SampleApp
    {
    public:
        explicit SampleApp(BackendType backend = kDefaultBackend, bool validation = true)
            : m_backendType(backend), m_validationEnabled(validation)
        {
        }
        virtual ~SampleApp() = default;

        SampleApp(const SampleApp&) = delete;
        SampleApp& operator=(const SampleApp&) = delete;

        int Run(int argc = 0, char** argv = nullptr);

    protected:
        // THE data root (resolved in Run: --data-root, else the Data/.dataroot walk from the
        // executable) and the mount over it - samples read engine data (shaders, fonts, models)
        // through these, never from a compiled-in path.
        [[nodiscard]] StringView DataRoot() const noexcept { return m_dataRoot.AsView(); }
        [[nodiscard]] vfs::IFileSystem& DataFileSystem() noexcept { return *m_dataFileSystem; }
        // A data-relative path resolved against the root ("Assets/fonts/x.ttf" -> absolute).
        [[nodiscard]] String DataPath(StringView relative) const
        {
            return vfs::DataPath(m_dataRoot.AsView(), relative);
        }

        virtual StringView Title() const { return u8"RHI Sample"; }
        virtual rhi::DeviceFeatures RequiredFeatures() const { return {}; }
        virtual rhi::TextureFormat SwapChainFormat() const
        {
            return rhi::TextureFormat::RGBA8UnormSrgb;
        }
        virtual rhi::PresentMode PresentMode() const { return rhi::PresentMode::Fifo; }
        virtual u32 BufferCount() const { return 2; }

        virtual Status OnInit() = 0;
        virtual void OnRender() = 0;
        virtual void OnResize(u32, u32) {}
        virtual void OnShutdown() = 0;

        shell::IShell* m_shell = nullptr; // borrowed from m_shellOwner
        shell::IWindow* m_window = nullptr;
        rhi::Backend* m_backend = nullptr;
        rhi::Device* m_device = nullptr;
        rhi::Queue* m_graphicsQueue = nullptr;
        rhi::Surface* m_surface = nullptr;
        rhi::SwapChain* m_swapChain = nullptr;

        u32 m_width = 1280;
        u32 m_height = 720;
        bool m_running = false;
        f32 m_deltaTime = 0.0f;
        f32 m_totalTime = 0.0f;

        void Exit() { m_running = false; }

        // === Screenshots ===
        // A sample calls CaptureBackbuffer once a frame, after it has drawn and before the
        // backbuffer goes to present; the framework arms it from F12 (to <user-data>/screenshots)
        // or the --screenshot flags (--screenshot <png> [--screenshot-frame N |
        // --screenshot-after S] [--screenshot-count N] [--screenshot-exit]), and writes the PNG
        // once the GPU has run the copy.

        /// Arms a screenshot of the next frame to `path` (empty: a new file under
        /// <user-data>/screenshots).
        void RequestScreenshot(StringView path = {});
        /// Records the armed screenshot's copy of `backbuffer` (the swapchain's current image, in
        /// RenderTarget) into `encoder`, leaving it as it found it.
        void CaptureBackbuffer(rhi::CommandEncoder& encoder, rhi::Texture* backbuffer);
        /// Whether a screenshot waits on frames being drawn: one armed, or a --screenshot run not
        /// yet done (it counts drawn frames). A sample that skips idle frames draws while it does.
        [[nodiscard]] bool ScreenshotPending() const noexcept
        {
            return m_screenshot.Armed() ||
                   (m_screenshotOptions.Requested() && m_screenshotsTaken < m_screenshotOptions.count);
        }

    private:
        BackendType m_backendType;
        bool m_validationEnabled;
        UniquePtr<shell::IShell> m_shellOwner;
        String m_dataRoot;
        UniquePtr<vfs::NativeFileSystem> m_dataFileSystem;

        rhi::ScreenshotCapture m_screenshot;
        rhi::ScreenshotOptions m_screenshotOptions;
        u64 m_renderedFrames = 0;
        u32 m_screenshotsTaken = 0;
        u32 m_screenshotSerial = 0;
        bool m_screenshotExitPending = false;

        void FinishScreenshot();
        Status Init();
        void MainLoop();
        void Shutdown();
        Status CreateBackend();
        Status CreateSwapChain();
        void CheckAndResize();
    };

    inline int SampleApp::Run(int argc, char** argv)
    {
        for (int i = 1; i < argc; ++i)
        {
            if (std::strcmp(argv[i], "--dx12") == 0 || std::strcmp(argv[i], "--d3d12") == 0)
            {
                m_backendType = BackendType::DX12;
            }
            else if (std::strcmp(argv[i], "--webgpu") == 0 || std::strcmp(argv[i], "--wgpu") == 0)
            {
                m_backendType = BackendType::WebGPU;
            }
            else if (std::strcmp(argv[i], "--vk") == 0 || std::strcmp(argv[i], "--vulkan") == 0)
            {
                m_backendType = BackendType::Vulkan;
            }
            else if (std::strcmp(argv[i], "--novalidation") == 0)
            {
                m_validationEnabled = false;
            }
        }

        m_screenshotOptions = rhi::ScreenshotOptionsFromArguments(argc, argv);
        m_dataRoot = vfs::ResolveDataRoot(argc, argv);
        if (m_dataRoot.IsEmpty())
        {
            std::fprintf(stderr, "SampleApp: no data root (put Data/ with its .dataroot marker "
                                 "beside the sample, or pass --data-root <dir>)\n");
            return 1;
        }
        m_dataFileSystem = MakeUnique<vfs::NativeFileSystem>(foundation::core::DefaultAllocator(),
                                                             m_dataRoot.AsView(),
                                                             foundation::core::DefaultAllocator());

        if (!Init().IsOk())
        {
            Shutdown();
            return 1;
        }
        MainLoop();
        Shutdown();
        return 0;
    }

    inline Status SampleApp::Init()
    {
        shell::WindowSettings ws{};
        ws.title = Title();
        ws.width = m_width;
        ws.height = m_height;

        m_shellOwner = shell::CreateShell(foundation::core::DefaultAllocator(), ws);
        m_shell = m_shellOwner.Get();
        if (m_shell == nullptr || m_shell->MainWindow() == nullptr)
        {
            rhi::LogError("SampleApp: shell/window init failed");
            return ErrorCode::Unknown;
        }
        m_window = m_shell->MainWindow();
        m_width = m_window->Width();
        m_height = m_window->Height();

        if (!CreateBackend().IsOk())
            return ErrorCode::Unknown;

        const shell::NativeWindow nw = m_window->Native();
        // Pass the shell's WSI so the RHI builds the matching surface (avoids the X11-handles-to-Wayland-WSI crash).
        rhi::SurfacePlatform plat = rhi::SurfacePlatform::Unknown;
        switch (nw.system)
        {
        case shell::WindowSystem::Win32:
            plat = rhi::SurfacePlatform::Win32;
            break;
        case shell::WindowSystem::X11:
            plat = rhi::SurfacePlatform::X11;
            break;
        case shell::WindowSystem::Wayland:
            plat = rhi::SurfacePlatform::Wayland;
            break;
        case shell::WindowSystem::Cocoa:
            plat = rhi::SurfacePlatform::Cocoa;
            break;
        default:
            break;
        }
        if (!m_backend->CreateSurface(nw.window, nw.display, m_surface, plat).IsOk())
        {
            rhi::LogError("SampleApp: createSurface failed");
            return ErrorCode::Unknown;
        }

        auto adapters = m_backend->EnumerateAdapters();
        if (adapters.Size() == 0)
        {
            rhi::LogError("SampleApp: no adapters");
            return ErrorCode::Unknown;
        }
        rhi::Adapter* adapter = adapters[0]; // best GPU first

        {
            rhi::AdapterInfo ai = adapter->Info();
            const char* backendName = m_backendType == BackendType::DX12     ? "DX12"
                                      : m_backendType == BackendType::WebGPU ? "WebGPU"
                                                                             : "Vulkan";
            const String name8 = String(ai.name);
            std::printf("SampleApp: backend=%s adapter=%s\n", backendName,
                        reinterpret_cast<const char*>(name8.CStr()));
        }

        rhi::DeviceDesc dd{};
        // Ask for a dedicated compute queue: the device clamps the request to what the adapter
        // has (0 on hardware without a compute-only family), so samples that check
        // GetQueueCount(Compute) - MultiQueue - see the real answer instead of the framework's
        // silence. Found by the Beef port: the desc never asked, so Sample017 always fell back.
        dd.computeQueueCount = 1;
        dd.graphicsQueueCount = 1;
        dd.requiredFeatures = RequiredFeatures();
        if (!adapter->CreateDevice(dd, m_device).IsOk())
        {
            rhi::LogError("SampleApp: createDevice failed");
            return ErrorCode::Unknown;
        }

        m_graphicsQueue = m_device->GetQueue(rhi::QueueType::Graphics);
        if (m_graphicsQueue == nullptr)
        {
            rhi::LogError("SampleApp: no graphics queue");
            return ErrorCode::Unknown;
        }

        if (!CreateSwapChain().IsOk())
            return ErrorCode::Unknown;
        return OnInit();
    }

    inline Status SampleApp::CreateBackend()
    {
        rhi::Backend* raw = nullptr;
        switch (m_backendType)
        {
        case BackendType::Vulkan:
        {
#ifdef OPTION_HAS_VULKAN
            rhi::vk::VkBackendDesc desc{};
            desc.enableValidation = m_validationEnabled;
            if (!rhi::vk::CreateBackend(desc, raw).IsOk())
            {
                rhi::LogError("SampleApp: rhi::vk::CreateBackend failed");
                return ErrorCode::Unknown;
            }
#else
            rhi::LogError("SampleApp: Vulkan backend not available in this build");
            return ErrorCode::Unknown;
#endif
            break;
        }
        case BackendType::DX12:
        {
#ifdef OPTION_HAS_DX12
            rhi::dx12::DxBackendDesc desc{};
            desc.enableValidation = m_validationEnabled;
            if (!rhi::dx12::CreateDxBackend(desc, raw).IsOk())
            {
                rhi::LogError("SampleApp: rhi::dx12::CreateDxBackend failed");
                return ErrorCode::Unknown;
            }
#else
            rhi::LogError("SampleApp: DX12 backend not available on this platform");
            return ErrorCode::Unknown;
#endif
            break;
        }
        case BackendType::WebGPU:
        {
#ifdef OPTION_HAS_WEBGPU
            rhi::webgpu::WebGpuBackendDesc desc{};
            if (!rhi::webgpu::CreateBackend(desc, raw, foundation::core::DefaultAllocator()).IsOk())
            {
                rhi::LogError("SampleApp: rhi::webgpu::CreateBackend failed (sidecar missing?)");
                return ErrorCode::Unknown;
            }
#else
            rhi::LogError("SampleApp: WebGPU backend not available in this build");
            return ErrorCode::Unknown;
#endif
            break;
        }
        }
        m_backend = m_validationEnabled ? rhi::validation::CreateValidatedBackend(raw) : raw;
        return m_backend != nullptr ? Status{} : Status{ErrorCode::Unknown};
    }

    inline Status SampleApp::CreateSwapChain()
    {
        rhi::SwapChainDesc desc{};
        desc.width = m_width;
        desc.height = m_height;
        desc.format = SwapChainFormat();
        desc.presentMode = PresentMode();
        desc.bufferCount = BufferCount();
        desc.label = u8"main";
        if (!m_device->CreateSwapChain(m_surface, desc, m_swapChain).IsOk())
        {
            rhi::LogError("SampleApp: createSwapChain failed");
            return ErrorCode::Unknown;
        }
        return Status{};
    }

    inline void SampleApp::MainLoop()
    {
        m_running = true;
        TimePoint lastTime = Clock::Now();

        while (m_running && m_shell->IsRunning())
        {
            m_shell->ProcessEvents();

            const TimePoint now = Clock::Now();
            m_deltaTime = (now - lastTime).AsSecondsF();
            lastTime = now;
            m_totalTime += m_deltaTime;

            if (!m_window->IsMinimized())
            {
                CheckAndResize();
                if (shell::IInputManager* input = m_shell->Input();
                    input != nullptr && input->Keyboard() != nullptr && input->Keyboard()->IsKeyPressed(shell::KeyCode::F12))
                {
                    RequestScreenshot();
                }
                OnRender();
                FinishScreenshot();
            }
        }
    }

    inline void SampleApp::RequestScreenshot(StringView path)
    {
        if (!path.IsEmpty())
        {
            m_screenshot.Request(path);
            return;
        }
        const String dir = PathJoin(GetUserDataDirectory().AsView(), u8"screenshots");
        if (!CreateDirectories(dir.AsView()))
        {
            rhi::LogError("SampleApp: could not create the screenshots directory");
            return;
        }
        const String file = PathJoin(dir.AsView(), Format(u8"sample-{}-{}.png", ProcessId(), ++m_screenshotSerial).AsView());
        m_screenshot.Request(file.AsView());
    }

    inline void SampleApp::CaptureBackbuffer(rhi::CommandEncoder& encoder, rhi::Texture* backbuffer)
    {
        ++m_renderedFrames;
        // The --screenshot run: from the due frame, one file a frame until --screenshot-count are
        // taken, and --screenshot-exit only after the last.
        if (m_screenshotOptions.Requested() && m_screenshotsTaken < m_screenshotOptions.count &&
            (m_screenshotsTaken > 0 || m_screenshotOptions.Due(m_renderedFrames, m_totalTime)))
        {
            m_screenshot.Request(m_screenshotOptions.PathFor(m_screenshotsTaken).AsView());
            ++m_screenshotsTaken;
            m_screenshotExitPending = m_screenshotOptions.exitAfter && m_screenshotsTaken == m_screenshotOptions.count;
        }
        if (!m_screenshot.Armed() || m_swapChain == nullptr)
            return;
        if (!m_screenshot.Record(*m_device, encoder, backbuffer, m_swapChain->Format(), m_width, m_height) &&
            m_screenshotExitPending)
        {
            std::fprintf(stderr, "SampleApp: the screenshot could not be taken (this surface cannot be copied)\n");
            Exit();
        }
    }

    // A screenshot recorded this frame: once the GPU has run it (a one-off, so wait for all),
    // map, write, and honour --screenshot-exit.
    inline void SampleApp::FinishScreenshot()
    {
        if (!m_screenshot.Recorded())
            return;
        m_device->WaitIdle();
        foundation::image::Image written;
        if (m_screenshot.Complete(*m_device, foundation::core::DefaultAllocator(), written).IsOk())
        {
            const String path(m_screenshot.Path());
            std::printf("SampleApp: screenshot written to %s\n", reinterpret_cast<const char*>(path.CStr()));
        }
        if (m_screenshotExitPending)
            Exit();
    }

    inline void SampleApp::CheckAndResize()
    {
        const u32 nw = m_window->Width();
        const u32 nh = m_window->Height();
        if (nw == 0 || nh == 0)
            return;
        if (nw == m_width && nh == m_height)
            return;
        m_width = nw;
        m_height = nh;
        m_device->WaitIdle();
        m_swapChain->Resize(m_width, m_height);
        OnResize(m_width, m_height);
    }

    inline void SampleApp::Shutdown()
    {
        if (m_device)
            m_device->WaitIdle();
        OnShutdown();
        if (m_device)
            m_screenshot.Release(*m_device);
        if (m_swapChain)
        {
            m_device->DestroySwapChain(m_swapChain);
            m_swapChain = nullptr;
        }
        if (m_surface)
        {
            m_device->DestroySurface(m_surface);
            m_surface = nullptr;
        }
        if (m_device)
        {
            m_device->Destroy();
            m_device = nullptr;
        }
        if (m_backend)
        {
            m_backend->Destroy();
            m_backend = nullptr;
        }
        m_shellOwner.Reset(); // destroys the window + shell
    }

} // namespace samples::framework
