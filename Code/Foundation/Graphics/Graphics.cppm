// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Graphics - the `foundation.graphics` module.
//
// The RHI render host that samples, the UI and the renderer share. Two pieces:
//
//   GraphicsDevice: the shared GPU. Backend (validation-wrapped), adapter, logical
//     device and graphics queue, plus the CPU frame-in-flight ring index. Created
//     once for the whole app; hands out RenderWindows.
//
//   RenderWindow: one window's presentation target. Surface, swapchain and a
//     per-window ring of command pools and fences. Created and destroyed at runtime
//     (the basis for detachable UI windows); the main window is just the first.
//
// A FrameContext is the per-window, per-frame hand-off the host gives a consumer:
// an acquired backbuffer and a host-created encoder. The host owns acquire, fence
// sync, submit, present and the backbuffer state transitions; the consumer records
// content (or calls BeginBackbufferPass for the common clear-and-pass case).
//
// Multi-window is uniform: there is no "main window" special case here. An app
// renders a list of RenderWindows, each independent, sharing one GraphicsDevice.

module;
#include "Core/Prelude.h"
#include "Profiler/Profiler.h" // PROFILE_SCOPE: a frame's two waits apart

export module foundation.graphics;

import foundation.core;
import foundation.profiler;
import foundation.rhi;
import foundation.shell;
// Backend factories live in sibling modules so this core host imports only the
// base RHI. That keeps it GPU-backend-agnostic and keeps heavy backend modules out
// of this interface, which GCC's module reader chokes on:
//   foundation.graphics.null: CreateNullGraphicsDevice (headless)
//   foundation.graphics.gpu: CreateGraphicsDevice (Vulkan/DX12)

namespace core = foundation::core;
using namespace foundation::shell; // IShell and the input/window types
namespace rhi = foundation::rhi;

export namespace foundation::graphics
{
    // Null is a real headless option (CI, servers, tests) that needs no GPU.
    enum class BackendType : core::u8
    {
        Vulkan,
        DX12,
        Null,
        WebGPU
    };

    /// The shared command-line backend scan: --vulkan/--vk, --dx12/--d3d12,
    /// --webgpu/--wgpu, --null-gpu; unrecognized arguments pass through untouched.
    /// One implementation for every executable (samples, sandboxes, tools).
    [[nodiscard]] inline BackendType
    SelectBackendFromArguments(int argc, char** argv,
                               BackendType fallback = BackendType::Vulkan)
    {
        const auto matches = [](const char* argument, const char* flag)
        {
            while (*argument != '\0' && *argument == *flag)
            {
                ++argument;
                ++flag;
            }
            return *argument == '\0' && *flag == '\0';
        };
        for (int i = 1; i < argc; ++i)
        {
            if (matches(argv[i], "--vulkan") || matches(argv[i], "--vk"))
            {
                return BackendType::Vulkan;
            }
            if (matches(argv[i], "--dx12") || matches(argv[i], "--d3d12"))
            {
                return BackendType::DX12;
            }
            if (matches(argv[i], "--webgpu") || matches(argv[i], "--wgpu"))
            {
                return BackendType::WebGPU;
            }
            if (matches(argv[i], "--null-gpu"))
            {
                return BackendType::Null;
            }
        }
        return fallback;
    }

    struct GraphicsDeviceDesc
    {
        BackendType backend = BackendType::Vulkan;
        // Validation (our RHI-layer wrapper and the backend's own layers, e.g. Vulkan validation)
        // defaults on for dev builds and off for optimized and shipping builds (BUILD_RELEASE, set
        // for Release, RelWithDebInfo and MinSizeRel), so a profiling run (RelWithDebInfo) measures
        // the real cost rather than the validation overhead. Override explicitly to force either way.
        // BUILD_RELEASE is always defined (0 in dev, 1 in optimized builds; see Core/Prelude.h), so
        // this must be `#if`, not `#ifdef` (which would always take the release branch).
#if BUILD_RELEASE
        bool enableValidation = false;
#else
        bool enableValidation = true;
#endif
        core::u32 framesInFlight = 2; // CPU-ahead ring depth
        rhi::DeviceFeatures requiredFeatures = {};
    };

    /// The shared command-line validation override: `--gpu-validation` forces the API
    /// validation layer on, `--no-gpu-validation` forces it off; neither leaves the desc's
    /// config default (on in Debug, off in optimized builds). The last one given wins.
    /// Every executable that creates a device through a desc should run its arguments
    /// through this rather than assign enableValidation itself.
    inline void ApplyValidationArguments(int argc, char** argv, GraphicsDeviceDesc& desc)
    {
        const auto matches = [](const char* argument, const char* flag)
        {
            while (*argument != '\0' && *argument == *flag)
            {
                ++argument;
                ++flag;
            }
            return *argument == '\0' && *flag == '\0';
        };
        for (int i = 1; i < argc; ++i)
        {
            if (matches(argv[i], "--gpu-validation"))
            {
                desc.enableValidation = true;
            }
            else if (matches(argv[i], "--no-gpu-validation"))
            {
                desc.enableValidation = false;
            }
        }
    }

    /// The device desc a desktop executable's command line asks for: the backend
    /// (SelectBackendFromArguments) and the validation layer (ApplyValidationArguments). The one
    /// call every entry makes, so a flag one executable honours, every executable honours.
    [[nodiscard]] inline GraphicsDeviceDesc DeviceDescFromArguments(int argc, char** argv)
    {
        GraphicsDeviceDesc desc;
        desc.backend = SelectBackendFromArguments(argc, argv);
        ApplyValidationArguments(argc, argv, desc);
        return desc;
    }

    struct RenderWindowDesc
    {
        rhi::TextureFormat format = rhi::TextureFormat::BGRA8UnormSrgb;
        rhi::PresentMode presentMode = rhi::PresentMode::Fifo;
        core::u32 bufferCount = 2; // swapchain images
    };

    class GraphicsDevice; // forward (RenderWindow refs it; defined complete below)
    class RenderWindow;   // forward (FrameContext refs it)

    // Typed per-window payload. The UI layer stashes its {RootView, VGContext,
    // VGRenderer} here without the host knowing the type (the capability As*() idiom).
    class IRenderWindowData
    {
    public:
        virtual ~IRenderWindowData() = default;
    };

    // Per-window, per-frame hand-off. Returned by RenderWindow::BeginFrame and
    // passed to consumers (Subsystem::Render / IApplicationModule::OnRenderWindow).
    struct FrameContext
    {
        bool valid = false;             // false: skip (minimized, or acquire failed)
        RenderWindow* window = nullptr; // which window this frame targets
        core::u32 frameIndex = 0;       // device ring index (0..framesInFlight-1)
        core::u32 width = 0;
        core::u32 height = 0;
        rhi::CommandEncoder* encoder = nullptr; // primary, host-created
        rhi::CommandPool* pool = nullptr;       // this frame's pool (extra encoders)
        rhi::Texture* backbuffer = nullptr;
        rhi::TextureView* backbufferView = nullptr;
        // The backbuffer viewed in its sRGB or UNORM twin (the window's swapchain asks for it);
        // null where the device or surface could not. VG's Encoded blend space draws through it.
        rhi::TextureView* backbufferAlternateView = nullptr;

        // Convenience for the common 2D/UI case: open a render pass that clears
        // and targets the backbuffer (the host already made the Undefined to
        // RenderTarget transition in BeginFrame). A RenderGraph-driven renderer
        // ignores this and records on `encoder` directly.
        rhi::RenderPassEncoder* BeginBackbufferPass(rhi::ClearColor clear)
        {
            return BeginBackbufferPass(clear, backbufferView);
        }
        // The same through a chosen view of the backbuffer (backbufferAlternateView).
        rhi::RenderPassEncoder* BeginBackbufferPass(rhi::ClearColor clear, rhi::TextureView* view)
        {
            rhi::ColorAttachment ca{};
            ca.view = view;
            ca.loadOp = rhi::LoadOp::Clear;
            ca.storeOp = rhi::StoreOp::Store;
            ca.clearValue = clear;
            rhi::RenderPassDesc rpd{};
            rpd.colorAttachments.Add(ca);
            m_pass = (encoder != nullptr) ? encoder->BeginRenderPass(rpd) : nullptr;
            return m_pass;
        }
        void EndBackbufferPass()
        {
            if (m_pass != nullptr)
            {
                m_pass->End();
                m_pass = nullptr;
            }
        }

        // One-call clear of the backbuffer (open a clear pass, close it). For
        // minimal apps that just want a visible, cleared window without touching
        // RHI types.
        void Clear(core::f32 r, core::f32 g, core::f32 b, core::f32 a = 1.0f)
        {
            rhi::ClearColor c;
            c.r = r;
            c.g = g;
            c.b = b;
            c.a = a;
            BeginBackbufferPass(c);
            EndBackbufferPass();
        }

    private:
        rhi::RenderPassEncoder* m_pass = nullptr;
    };

    // A window's presentation target. Owns its surface and swapchain and a per-frame
    // ring of command pools and fences (per-window present sync). Destroyed via the
    // GraphicsDevice that made it.
    class RenderWindow
    {
    public:
        // Built by GraphicsDevice::CreateRenderWindow; takes ownership of the RHI
        // objects. Arrays are sized to framesInFlight.
        RenderWindow(GraphicsDevice& device, IWindow& window, rhi::Surface* surface,
                     rhi::SwapChain* swapChain, core::Array<rhi::CommandPool*>&& pools,
                     core::Array<rhi::Fence*>&& fences) noexcept
            : m_device(&device), m_window(&window), m_surface(surface), m_swapChain(swapChain),
              m_pools(static_cast<core::Array<rhi::CommandPool*>&&>(pools)),
              m_fences(static_cast<core::Array<rhi::Fence*>&&>(fences)), m_width(window.PixelWidth()),
              m_height(window.PixelHeight())
        {
            m_fenceValues.Resize(m_fences.Size(), 0ull);
        }

        ~RenderWindow();

        RenderWindow(const RenderWindow&) = delete;
        RenderWindow& operator=(const RenderWindow&) = delete;

        [[nodiscard]] IWindow& Window() noexcept { return *m_window; }
        [[nodiscard]] rhi::SwapChain* Swap() noexcept { return m_swapChain; }

        // Poll the window size; recreate the swapchain if it changed. Returns true
        // when a resize happened.
        bool SyncSize();

        // Acquire this window's backbuffer and open a host-created encoder from the
        // current frame's pool (after the per-window fence guards reuse). The
        // returned FrameContext is invalid when the window is minimized or zero-sized,
        // or acquisition fails; the caller skips rendering it this frame.
        FrameContext BeginFrame();

        // Transition the backbuffer to Present, submit (signalling the per-window
        // fence), and present. No-op for an invalid frame.
        void EndFrame(FrameContext& frame);

        [[nodiscard]] IRenderWindowData* Data() noexcept { return m_data.Get(); }
        void SetData(core::UniquePtr<IRenderWindowData> data) noexcept
        {
            m_data = static_cast<core::UniquePtr<IRenderWindowData>&&>(data);
        }

    private:
        GraphicsDevice* m_device;               // borrowed
        IWindow* m_window;                      // borrowed
        rhi::Surface* m_surface;                // owned
        rhi::SwapChain* m_swapChain;            // owned
        core::Array<rhi::CommandPool*> m_pools; // owned, one per frame-in-flight
        core::Array<rhi::Fence*> m_fences;      // owned, one per frame-in-flight
        core::Array<core::u64> m_fenceValues;
        core::u32 m_width;
        core::u32 m_height;
        core::UniquePtr<IRenderWindowData> m_data; // optional typed payload
    };

    class GraphicsDevice
    {
    public:
        // Build a GraphicsDevice from an already-created backend (takes
        // ownership): enumerate adapters, create the logical device and graphics
        // queue. Backend-agnostic: GPU backends (Vulkan/DX12) are built by the
        // `foundation.graphics.gpu` factory, which then calls this. On failure
        // the backend is destroyed.
        static core::Result<core::UniquePtr<GraphicsDevice>>
        FromBackend(rhi::Backend* backend, core::u32 framesInFlight,
                    const rhi::DeviceFeatures& features = {})
        {
            if (backend == nullptr)
            {
                rhi::LogError("GraphicsDevice::FromBackend: null backend (creation failed)");
                return core::Err(core::ErrorCode::Unknown);
            }

            core::Span<rhi::Adapter* const> adapters = backend->EnumerateAdapters();
            if (adapters.Size() == 0)
            {
                rhi::LogError("GraphicsDevice::FromBackend: no adapters enumerated");
                backend->Destroy();
                return core::Err(core::ErrorCode::Unknown);
            }

            rhi::DeviceDesc dd{};
            dd.graphicsQueueCount = 1;
            dd.requiredFeatures = features;
            rhi::Device* device = nullptr;
            if (!adapters[0]->CreateDevice(dd, device).IsOk())
            {
                rhi::LogError("GraphicsDevice::FromBackend: CreateDevice failed");
                backend->Destroy();
                return core::Err(core::ErrorCode::Unknown);
            }

            rhi::Queue* queue = device->GetQueue(rhi::QueueType::Graphics);
            if (queue == nullptr)
            {
                rhi::LogError("GraphicsDevice::FromBackend: no graphics queue");
                device->Destroy();
                backend->Destroy();
                return core::Err(core::ErrorCode::Unknown);
            }

#if PLATFORM_WEB
            // Web (Emscripten/Dawn) surfaces hand out a single current texture, reused every frame,
            // and wgpuQueueSubmit validates asynchronously. With more than one frame in flight, the
            // next frame's AcquireNextImage releases that shared surface texture while the prior
            // frame's still-pending submit references it ("Destroyed texture used in a submit"), and
            // Dawn silently drops that submit, one-shot work such as an IBL bake included. Serialize
            // to a single frame in flight: BeginFrame's fence wait then guarantees a frame's submit
            // has completed before the next acquire releases the surface texture.
            const core::u32 frames = 1u;
            (void)framesInFlight;
#else
            const core::u32 frames = framesInFlight == 0 ? 1 : framesInFlight;
#endif
            auto gd = core::MakeUnique<GraphicsDevice>(core::DefaultAllocator(), backend, device,
                                                       queue, frames);
            gd->m_adapter = adapters[0]->Info();
            return core::Result<core::UniquePtr<GraphicsDevice>>(
                static_cast<core::UniquePtr<GraphicsDevice>&&>(gd));
        }

        GraphicsDevice(rhi::Backend* backend, rhi::Device* device, rhi::Queue* queue,
                       core::u32 framesInFlight) noexcept
            : m_backend(backend), m_device(device), m_queue(queue), m_framesInFlight(framesInFlight)
        {
        }

        ~GraphicsDevice()
        {
            if (m_device != nullptr)
            {
                m_device->WaitIdle();
                m_device->Destroy();
                m_device = nullptr;
            }
            if (m_backend != nullptr)
            {
                m_backend->Destroy();
                m_backend = nullptr;
            }
        }

        GraphicsDevice(const GraphicsDevice&) = delete;
        GraphicsDevice& operator=(const GraphicsDevice&) = delete;

        // Create a presentation target (surface, swapchain, per-frame pools and
        // fences) for a window. The window must outlive the RenderWindow.
        core::Result<core::UniquePtr<RenderWindow>> CreateRenderWindow(IWindow& window,
                                                                       const RenderWindowDesc& desc)
        {
            const NativeWindow nw = window.Native();
            // Tell the RHI which WSI the shell used so it creates the matching surface (no guessing:
            // feeding X11 handles to the Wayland WSI segfaults). The shell's WindowSystem maps 1:1.
            rhi::SurfacePlatform plat = rhi::SurfacePlatform::Unknown;
            switch (nw.system)
            {
            case WindowSystem::Win32:
                plat = rhi::SurfacePlatform::Win32;
                break;
            case WindowSystem::X11:
                plat = rhi::SurfacePlatform::X11;
                break;
            case WindowSystem::Wayland:
                plat = rhi::SurfacePlatform::Wayland;
                break;
            case WindowSystem::Cocoa:
                plat = rhi::SurfacePlatform::Cocoa;
                break;
            default:
                break;
            }
            rhi::Surface* surface = nullptr;
            if (!m_backend->CreateSurface(nw.window, nw.display, surface, plat).IsOk())
            {
                return core::Err(core::ErrorCode::Unknown);
            }

            rhi::SwapChainDesc sd{};
            sd.width = window.PixelWidth(); // the drawable's pixels, not the OS's window units
            sd.height = window.PixelHeight();
            sd.format = desc.format;
            sd.presentMode = desc.presentMode;
            sd.bufferCount = desc.bufferCount;
            // Viewable in its sRGB or UNORM twin too, for UI that blends encoded values (VG's
            // Encoded blend space); the swapchain says whether it could.
            sd.alternateViewFormat = rhi::SrgbTwin(desc.format);
            sd.label = u8"renderwindow";
            rhi::SwapChain* swapChain = nullptr;
            if (!m_device->CreateSwapChain(surface, sd, swapChain).IsOk())
            {
                m_device->DestroySurface(surface);
                return core::Err(core::ErrorCode::Unknown);
            }

            core::Array<rhi::CommandPool*> pools;
            core::Array<rhi::Fence*> fences;
            for (core::u32 i = 0; i < m_framesInFlight; ++i)
            {
                rhi::CommandPool* pool = nullptr;
                rhi::Fence* fence = nullptr;
                if (!m_device->CreateCommandPool(rhi::QueueType::Graphics, pool).IsOk() ||
                    !m_device->CreateFence(0, fence).IsOk())
                {
                    for (rhi::CommandPool* p : pools)
                    {
                        m_device->DestroyCommandPool(p);
                    }
                    for (rhi::Fence* f : fences)
                    {
                        m_device->DestroyFence(f);
                    }
                    if (pool != nullptr)
                    {
                        m_device->DestroyCommandPool(pool);
                    }
                    m_device->DestroySwapChain(swapChain);
                    m_device->DestroySurface(surface);
                    return core::Err(core::ErrorCode::Unknown);
                }
                pools.PushBack(pool);
                fences.PushBack(fence);
            }

            auto rw = core::MakeUnique<RenderWindow>(
                core::DefaultAllocator(), *this, window, surface, swapChain,
                static_cast<core::Array<rhi::CommandPool*>&&>(pools),
                static_cast<core::Array<rhi::Fence*>&&>(fences));
            return core::Result<core::UniquePtr<RenderWindow>>(
                static_cast<core::UniquePtr<RenderWindow>&&>(rw));
        }

        [[nodiscard]] rhi::Device* Raw() noexcept { return m_device; }
        [[nodiscard]] rhi::Queue* GfxQueue() noexcept { return m_queue; }
        /// The adapter the device was made on: its name, vendor and type (a CPU adapter draws in
        /// software, many times slower than a GPU).
        [[nodiscard]] const rhi::AdapterInfo& Adapter() const noexcept { return m_adapter; }
        [[nodiscard]] core::u32 FramesInFlight() const noexcept { return m_framesInFlight; }
        [[nodiscard]] core::u32 CurrentFrame() const noexcept { return m_currentFrame; }

        // Advance the CPU frame ring once per app frame (after all windows are
        // rendered). Consumers key per-frame GPU resources on CurrentFrame().
        void AdvanceFrame() noexcept { m_currentFrame = (m_currentFrame + 1) % m_framesInFlight; }

    private:
        rhi::Backend* m_backend; // owned
        rhi::AdapterInfo m_adapter;
        rhi::Device* m_device;   // owned
        rhi::Queue* m_queue;     // borrowed from device
        core::u32 m_framesInFlight;
        core::u32 m_currentFrame = 0;
    };

    // ----- RenderWindow out-of-line defs (need GraphicsDevice complete) -----

    inline RenderWindow::~RenderWindow()
    {
        rhi::Device* dev = m_device->Raw();
        if (dev != nullptr)
        {
            dev->WaitIdle();
        }
        for (rhi::CommandPool* p : m_pools)
        {
            if (p != nullptr)
            {
                dev->DestroyCommandPool(p);
            }
        }
        for (rhi::Fence* f : m_fences)
        {
            if (f != nullptr)
            {
                dev->DestroyFence(f);
            }
        }
        if (m_swapChain != nullptr)
        {
            dev->DestroySwapChain(m_swapChain);
            m_swapChain = nullptr;
        }
        if (m_surface != nullptr)
        {
            dev->DestroySurface(m_surface);
            m_surface = nullptr;
        }
    }

    inline bool RenderWindow::SyncSize()
    {
        const core::u32 nw = m_window->PixelWidth();
        const core::u32 nh = m_window->PixelHeight();
        if (nw == 0 || nh == 0)
        {
            return false;
        }
        if (nw == m_width && nh == m_height)
        {
            return false;
        }
        m_width = nw;
        m_height = nh;
        m_device->Raw()->WaitIdle();
        m_swapChain->Resize(nw, nh);
        return true;
    }

    inline FrameContext RenderWindow::BeginFrame()
    {
        if (m_window->IsMinimized() || m_window->Width() == 0 || m_window->Height() == 0)
        {
            return FrameContext{};
        }
        // After device loss (GPU hang / TDR, driver reset) stop rendering: continuing
        // spins through failing acquire/submit calls, flooding stderr and stalling the
        // OS. The device is the authority (Device::IsLost); loss is sticky, so the
        // window stays dark until the app recreates the device.
        if (m_device->Raw() == nullptr || m_device->Raw()->IsLost())
        {
            return FrameContext{};
        }

        const core::u32 fi = m_device->CurrentFrame();
        // Guard reuse of this slot's pool/backbuffer: wait the GPU's last
        // submission against this window's fence at this ring slot.
        // Timed apart, as their causes differ: the fence is the GPU still on the frame that last
        // used this slot (ours to fix); the image is the compositor not yet giving one back (vsync,
        // or the display stalled).
        if (m_fenceValues[fi] > 0)
        {
            PROFILE_SCOPE("Render.Acquire.Fence");
            m_fences[fi]->Wait(m_fenceValues[fi], ~0ull);
        }

        {
            PROFILE_SCOPE("Render.Acquire.Image");
            if (!m_swapChain->AcquireNextImage().IsOk())
            {
                return FrameContext{};
            }
        }

        m_pools[fi]->Reset();
        rhi::CommandEncoder* enc = nullptr;
        if (!m_pools[fi]->CreateEncoder(enc).IsOk() || enc == nullptr)
        {
            return FrameContext{};
        }

        // Host-managed backbuffer transition (Undefined -> RenderTarget).
        enc->TransitionTexture(m_swapChain->CurrentTexture(), rhi::ResourceState::Undefined,
                               rhi::ResourceState::RenderTarget);

        FrameContext f;
        f.valid = true;
        f.window = this;
        f.frameIndex = fi;
        // The frame's dimensions are the backbuffer's (the swapchain's configured size), not the
        // live window size: on web the canvas can resize between SyncSize and here, and every
        // downstream consumer (viewports, scissors, post targets) must agree with the attachment
        // this frame actually renders into. WebGPU validation rejects the whole command buffer
        // on any out-of-attachment viewport or scissor.
        f.width = m_swapChain->Width();
        f.height = m_swapChain->Height();
        f.encoder = enc;
        f.pool = m_pools[fi];
        f.backbuffer = m_swapChain->CurrentTexture();
        f.backbufferView = m_swapChain->CurrentTextureView();
        f.backbufferAlternateView = m_swapChain->CurrentAlternateTextureView();
        return f;
    }

    inline void RenderWindow::EndFrame(FrameContext& frame)
    {
        if (!frame.valid)
        {
            return;
        }
        const core::u32 fi = frame.frameIndex;

        frame.encoder->TransitionTexture(m_swapChain->CurrentTexture(),
                                         rhi::ResourceState::RenderTarget,
                                         rhi::ResourceState::Present);
        rhi::CommandBuffer* cb = frame.encoder->Finish();

        ++m_fenceValues[fi];
        rhi::CommandBuffer* cbs[1] = {cb};
        m_device->GfxQueue()->Submit(core::Span<rhi::CommandBuffer* const>(cbs, 1), m_fences[fi],
                                     m_fenceValues[fi]);

        m_swapChain->Present(m_device->GfxQueue());
        m_pools[fi]->DestroyEncoder(frame.encoder);
        frame.encoder = nullptr;
    }
}
