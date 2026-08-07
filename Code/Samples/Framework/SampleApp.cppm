// SampleApp - abstract base for RHI samples (adapted from the Draconic sample
// framework to Draconic's shell). Brings up a window (draconic.shell),
// a Vulkan backend (validation-wrapped), device, queue, and swap chain; pumps
// events, tracks timing, and calls OnRender(). Resize is detected by polling the
// window size; the loop skips rendering while minimized.

module;
#include "Draconic.Foundation/Prelude.h"
#include <cstdio>
#include <cstring>

export module draconic.samples.framework:sample_app;

import draconic.foundation;
import draconic.rhi;
import draconic.rhi.vulkan;
#ifdef DRACONIC_HAS_DX12
import draconic.rhi.dx12;
#endif
#ifdef DRACONIC_HAS_WEBGPU
import draconic.rhi.webgpu;
#endif
import draconic.rhi.validation;
import draconic.shell;
import draconic.shell.desktop;

using namespace draconic::foundation;

export namespace draconic::samples::framework
{

    enum class BackendType
    {
        Vulkan,
        DX12,
        WebGPU
    };

    class SampleApp
    {
    public:
        explicit SampleApp(BackendType backend = BackendType::Vulkan, bool validation = true)
            : m_backendType(backend), m_validationEnabled(validation)
        {
        }
        virtual ~SampleApp() = default;

        SampleApp(const SampleApp&) = delete;
        SampleApp& operator=(const SampleApp&) = delete;

        int Run(int argc = 0, char** argv = nullptr);

    protected:
        virtual StringView Title() const { return u8"Draconic Sample"; }
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

    private:
        BackendType m_backendType;
        bool m_validationEnabled;
        UniquePtr<shell::IShell> m_shellOwner;

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

        m_shellOwner = shell::CreateShell(ws);
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
            rhi::vk::VkBackendDesc desc{};
            desc.enableValidation = m_validationEnabled;
            if (!rhi::vk::CreateBackend(desc, raw).IsOk())
            {
                rhi::LogError("SampleApp: rhi::vk::CreateBackend failed");
                return ErrorCode::Unknown;
            }
            break;
        }
        case BackendType::DX12:
        {
#ifdef DRACONIC_HAS_DX12
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
#ifdef DRACONIC_HAS_WEBGPU
            rhi::webgpu::WebGpuBackendDesc desc{};
            if (!rhi::webgpu::CreateBackend(desc, raw).IsOk())
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
                OnRender();
            }
        }
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

} // namespace draconic::samples::framework
