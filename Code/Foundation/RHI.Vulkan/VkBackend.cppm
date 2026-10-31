// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Vulkan implementation of Backend.
/// Creates VkInstance, enumerates physical devices, creates surfaces.

module;
#include "Core/Prelude.h"

#include "VkIncludes.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// VkApplicationInfo identity strings; CMake bakes these from the project name on the RHI.Vulkan
// target. They are required, with no in-source default, so the name lives only in the build system.
#ifndef VULKAN_APPLICATION_NAME
#error "VULKAN_APPLICATION_NAME is not defined - set it on the RHI.Vulkan target (from ${PROJECT_NAME})"
#endif
#ifndef VULKAN_ENGINE_NAME
#error "VULKAN_ENGINE_NAME is not defined - set it on the RHI.Vulkan target (from ${PROJECT_NAME})"
#endif

export module foundation.rhi.vulkan:backend;

import foundation.core;
import foundation.rhi;
import :adapter;
import :surface;

using namespace foundation::core;

// Linux surface types, forward-declared to avoid header pollution. They sit in the module purview
// (not the GMF) because GCC requires the GMF to contain only #includes.
#if defined(__linux__)
extern "C"
{
    typedef struct _XDisplay Display;
}
typedef unsigned long XID;

struct VkXlibSurfaceCreateInfoKHR
{
    VkStructureType sType;
    const void* pNext;
    VkFlags flags;
    Display* dpy;
    XID window;
};
#define VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR static_cast<VkStructureType>(1000004000)
#define VK_KHR_XLIB_SURFACE_EXTENSION_NAME "VK_KHR_xlib_surface"
using PFN_vkCreateXlibSurfaceKHR = VkResult(VKAPI_PTR*)(VkInstance,
                                                        const VkXlibSurfaceCreateInfoKHR*,
                                                        const VkAllocationCallbacks*,
                                                        VkSurfaceKHR*);

extern "C"
{
    struct wl_display;
    struct wl_surface;
}

struct VkWaylandSurfaceCreateInfoKHR
{
    VkStructureType sType;
    const void* pNext;
    VkFlags flags;
    wl_display* display;
    wl_surface* surface;
};
#define VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR static_cast<VkStructureType>(1000006000)
#define VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME "VK_KHR_wayland_surface"
using PFN_vkCreateWaylandSurfaceKHR = VkResult(VKAPI_PTR*)(VkInstance,
                                                           const VkWaylandSurfaceCreateInfoKHR*,
                                                           const VkAllocationCallbacks*,
                                                           VkSurfaceKHR*);
#endif // __linux__

namespace foundation::rhi::vk
{
    // Every validation error the debug messenger saw, process-wide. The messenger is per backend
    // but the count is one: tests read it around a probe, so a stage-interface or layout mistake
    // that the driver tolerates still fails the test instead of only printing.
    std::atomic<u32> g_validationErrorCount{0};
} // namespace foundation::rhi::vk

export namespace foundation::rhi::vk
{
    /// The number of Vulkan validation-layer errors reported so far in this process (0 when
    /// validation is off or the layer is not installed). Tests snapshot it before and compare
    /// after.
    [[nodiscard]] u32 ValidationErrorCount() noexcept
    {
        return g_validationErrorCount.load(std::memory_order_relaxed);
    }

    /// Configuration for VK backend creation.
    struct VkBackendDesc
    {
        bool enableValidation = false;
    };

    /// Vulkan implementation of Backend.
    class VkBackendImpl : public Backend
    {
    public:
        explicit VkBackendImpl(IAllocator& allocator) noexcept : m_allocator(allocator) {}
        ~VkBackendImpl() override { destroyImpl(); }

        friend Status CreateBackend(const VkBackendDesc&, Backend*&, IAllocator&);

        // ---- Backend interface ----

        Span<Adapter* const> EnumerateAdapters() override
        {
            return Span<Adapter* const>(m_adapterPtrs.Data(), m_adapterPtrs.Size());
        }

        Status
        CreateSurface(void* windowHandle,
                      void*
#if PLATFORM_LINUX
                          displayHandle
#else
        /*displayHandle*/
#endif
                      ,
                      Surface*& out,
                      [[maybe_unused]] SurfacePlatform platform = SurfacePlatform::Unknown) override
        {
            out = nullptr;
            if (!windowHandle)
            {
                LogError("VkBackend: window handle is null");
                return ErrorCode::InvalidArgument;
            }

            VkSurfaceKHR vkSurface = VK_NULL_HANDLE;

#ifdef _WIN32
            VkWin32SurfaceCreateInfoKHR ci{};
            ci.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
            ci.hinstance = ::GetModuleHandleW(nullptr);
            ci.hwnd = reinterpret_cast<HWND>(windowHandle);

            VkResult vr = vkCreateWin32SurfaceKHR(m_instance, &ci, nullptr, &vkSurface);
            if (vr != VK_SUCCESS)
            {
                LogErrorf("VkBackend: vkCreateWin32SurfaceKHR failed (%d)", static_cast<int>(vr));
                return ErrorCode::Unknown;
            }
#elif defined(__linux__)
            // Create the surface type the shell used. `platform` is authoritative (it comes from
            // the SDL video driver that created the window); feeding handles to the wrong WSI, such
            // as an X11 Display* to vkCreateWaylandSurfaceKHR, dereferences a non-Wayland pointer
            // and segfaults deep in the driver (the classic "force X11 under a Wayland session"
            // crash). Only when the caller did not specify a platform (Unknown) do we fall back to
            // the WAYLAND_DISPLAY heuristic.
            VkResult vr = VK_ERROR_INITIALIZATION_FAILED;
            bool triedWayland = false, triedX11 = false;

            SurfacePlatform effective = platform;
            if (effective == SurfacePlatform::Unknown)
            {
                effective = (m_hasWayland && std::getenv("WAYLAND_DISPLAY"))
                                ? SurfacePlatform::Wayland
                                : SurfacePlatform::X11;
            }

            // Independent record of what the RHI is about to create (compare against the shell's log line).
            {
                const char* name = (effective == SurfacePlatform::Wayland) ? "Wayland"
                                   : (effective == SurfacePlatform::X11)   ? "X11"
                                   : (effective == SurfacePlatform::Win32) ? "Win32"
                                   : (effective == SurfacePlatform::Cocoa) ? "Cocoa"
                                                                           : "Unknown";
                LogInfof("[RHI] Vulkan surface WSI: %s (%s)", name,
                         platform == SurfacePlatform::Unknown ? "guessed" : "from shell");
            }

            if (effective == SurfacePlatform::Wayland && m_hasWayland)
            {
                auto fn = reinterpret_cast<PFN_vkCreateWaylandSurfaceKHR>(
                    vkGetInstanceProcAddr(m_instance, "vkCreateWaylandSurfaceKHR"));
                if (fn)
                {
                    VkWaylandSurfaceCreateInfoKHR ci{};
                    ci.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR;
                    ci.display = reinterpret_cast<wl_display*>(displayHandle);
                    ci.surface = reinterpret_cast<wl_surface*>(windowHandle);
                    vr = fn(m_instance, &ci, nullptr, &vkSurface);
                    triedWayland = true;
                }
            }
            else if (effective == SurfacePlatform::X11 && m_hasXlib)
            {
                auto fn = reinterpret_cast<PFN_vkCreateXlibSurfaceKHR>(
                    vkGetInstanceProcAddr(m_instance, "vkCreateXlibSurfaceKHR"));
                if (fn)
                {
                    VkXlibSurfaceCreateInfoKHR ci{};
                    ci.sType = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR;
                    ci.dpy = reinterpret_cast<Display*>(displayHandle);
                    ci.window = static_cast<XID>(reinterpret_cast<std::uintptr_t>(windowHandle));
                    vr = fn(m_instance, &ci, nullptr, &vkSurface);
                    triedX11 = true;
                }
            }

            if (vr != VK_SUCCESS)
            {
                LogErrorf(
                    "VkBackend: surface creation failed (platform=%d, tried wayland=%d, x11=%d)",
                    static_cast<int>(effective), triedWayland, triedX11);
                return ErrorCode::Unknown;
            }
            if (vr != VK_SUCCESS)
            {
                LogErrorf("VkBackend: surface creation failed (%d)", static_cast<int>(vr));
                return ErrorCode::Unknown;
            }
#else
            (void)displayHandle;
            LogError("VkBackend: surface creation not supported on this platform");
            return ErrorCode::NotSupported;
#endif

            out = m_allocator.New<VkSurfaceImpl>(vkSurface, m_instance);
            return ErrorCode::Ok;
        }

        void Destroy() override
        {
            destroyImpl();
            IAllocator& alloc = m_allocator;
            this->~VkBackendImpl();
            alloc.Free(this);
        }

        // ---- Internal ----

        [[nodiscard]] VkInstance instance() const { return m_instance; }
        [[nodiscard]] bool validationEnabled() const { return m_validationEnabled; }
        [[nodiscard]] IAllocator& allocator() const noexcept { return m_allocator; }

    private:
        friend Status CreateBackend(const VkBackendDesc& desc, Backend*& out);

        Status init(bool enableValidation)
        {
            m_validationEnabled = enableValidation;
            // Announce validation state so a perf run can confirm the layers are off: the layer
            // validates every command, so a profile taken with it on measures the layer.
            if (enableValidation)
            {
                std::fprintf(stderr,
                             "[Vulkan] validation layers: ENABLED - this build has device "
                             "validation enabled, which is known to be slow. For real numbers "
                             "use a build with device validation disabled (RelWithDebInfo, or "
                             "--no-gpu-validation).\n");
            }
            else
            {
                std::fprintf(stderr, "[Vulkan] validation layers: DISABLED\n");
            }

            // ---- Application info ----
            VkApplicationInfo appInfo{};
            appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
            appInfo.pApplicationName = VULKAN_APPLICATION_NAME;
            appInfo.applicationVersion = VK_MAKE_API_VERSION(0, 1, 0, 0);
            appInfo.pEngineName = VULKAN_ENGINE_NAME;
            appInfo.engineVersion = VK_MAKE_API_VERSION(0, 1, 0, 0);
            appInfo.apiVersion = VK_API_VERSION_1_3;

            // ---- Extensions ----
            Array<const char*> extensions;
            extensions.PushBack(VK_KHR_SURFACE_EXTENSION_NAME);

#ifdef _WIN32
            extensions.PushBack(VK_KHR_WIN32_SURFACE_EXTENSION_NAME);
#elif defined(__linux__)
            {
                u32 availCount = 0;
                vkEnumerateInstanceExtensionProperties(nullptr, &availCount, nullptr);
                Array<VkExtensionProperties> avail(availCount);
                vkEnumerateInstanceExtensionProperties(nullptr, &availCount, avail.Data());
                auto hasExt = [&](const char* name)
                {
                    for (const auto& e : avail)
                        if (std::strcmp(e.extensionName, name) == 0)
                            return true;
                    return false;
                };

                bool hasXlib = hasExt(VK_KHR_XLIB_SURFACE_EXTENSION_NAME);
                bool hasWayland = hasExt(VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME);

                // Enable both surface extensions if available. The actual surface
                // type is detected at createSurface time from the window handles,
                // because SDL3 may choose Wayland even when DISPLAY is set.
                if (hasXlib)
                    extensions.PushBack(VK_KHR_XLIB_SURFACE_EXTENSION_NAME);
                if (hasWayland)
                    extensions.PushBack(VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME);
                m_hasXlib = hasXlib;
                m_hasWayland = hasWayland;
                if (!hasXlib && !hasWayland)
                {
                    LogError("VkBackend: no surface extension available (need xlib or wayland)");
                    return ErrorCode::Unknown;
                }
            }
#endif

            // Debug utils whenever the loader has it, not only with validation: frame captures read
            // pass names from it, and the encoder's label calls do nothing without it (they
            // null-check their entry points). Validation still needs it for the messenger, so that
            // case pushes it regardless.
            {
                u32 availCount = 0;
                vkEnumerateInstanceExtensionProperties(nullptr, &availCount, nullptr);
                Array<VkExtensionProperties> avail(availCount);
                vkEnumerateInstanceExtensionProperties(nullptr, &availCount, avail.Data());
                bool hasDebugUtils = false;
                for (const auto& e : avail)
                    if (std::strcmp(e.extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == 0)
                        hasDebugUtils = true;
                if (enableValidation || hasDebugUtils)
                    extensions.PushBack(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
            }

            // ---- Layers ----
            Array<const char*> layers;
            if (enableValidation)
                layers.PushBack("VK_LAYER_KHRONOS_validation");

            // ---- Create instance ----
            VkInstanceCreateInfo ci{};
            ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
            ci.pApplicationInfo = &appInfo;
            ci.enabledExtensionCount = static_cast<u32>(extensions.Size());
            ci.ppEnabledExtensionNames = extensions.Data();
            ci.enabledLayerCount = static_cast<u32>(layers.Size());
            ci.ppEnabledLayerNames = layers.Data();

            VkResult vr = vkCreateInstance(&ci, nullptr, &m_instance);
            if (vr != VK_SUCCESS)
            {
                LogErrorf("VkBackend: vkCreateInstance failed (%d)", static_cast<int>(vr));
                return ErrorCode::Unknown;
            }

            if (enableValidation)
                setupDebugMessenger();

            enumeratePhysicalDevices();

            isInitialized = true;
            return ErrorCode::Ok;
        }

        void setupDebugMessenger()
        {
            VkDebugUtilsMessengerCreateInfoEXT ci{};
            ci.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
            ci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                 VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            ci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                             VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                             VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            ci.pfnUserCallback = &debugCallback;

            auto pfn = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(m_instance, "vkCreateDebugUtilsMessengerEXT"));
            if (pfn)
                pfn(m_instance, &ci, nullptr, &m_debugMessenger);
        }

        static VKAPI_ATTR VkBool32 VKAPI_CALL
        debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                      VkDebugUtilsMessageTypeFlagsEXT /*types*/,
                      const VkDebugUtilsMessengerCallbackDataEXT* data, void* /*userData*/)
        {
            // Print straight to stderr (not only LogErrorf, whose sink may be swallowed) so
            // validation messages are visible in dev builds.
            if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
            {
                g_validationErrorCount.fetch_add(1, std::memory_order_relaxed);
                std::fprintf(stderr, "[Vulkan ERROR] %s\n", data->pMessage);
                LogErrorf("[Vulkan ERROR] %s", data->pMessage);
            }
            else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
            {
                std::fprintf(stderr, "[Vulkan WARN] %s\n", data->pMessage);
                LogWarningf("[Vulkan WARN] %s", data->pMessage);
            }
            return VK_FALSE;
        }

        void enumeratePhysicalDevices()
        {
            u32 count = 0;
            vkEnumeratePhysicalDevices(m_instance, &count, nullptr);
            if (count == 0)
                return;

            Array<VkPhysicalDevice> devices(count);
            vkEnumeratePhysicalDevices(m_instance, &count, devices.Data());

            m_adapters.Reserve(count);
            m_adapterPtrs.Reserve(count);
            for (VkPhysicalDevice pd : devices)
            {
                auto* a = m_allocator.New<VkAdapterImpl>(pd, m_instance, m_allocator);
                m_adapters.PushBack(a);
                m_adapterPtrs.PushBack(a);
            }

            // Expose adapters best-GPU-first; callers take [0]. See Backend::EnumerateAdapters.
            SortAdaptersByPreference(m_adapterPtrs);
        }

        void destroyImpl()
        {
            for (auto* a : m_adapters)
                m_allocator.Delete(a);
            m_adapters.Clear();
            m_adapterPtrs.Clear();

            if (m_debugMessenger != VK_NULL_HANDLE)
            {
                auto pfn = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                    vkGetInstanceProcAddr(m_instance, "vkDestroyDebugUtilsMessengerEXT"));
                if (pfn)
                    pfn(m_instance, m_debugMessenger, nullptr);
                m_debugMessenger = VK_NULL_HANDLE;
            }

            if (m_instance != VK_NULL_HANDLE)
            {
                vkDestroyInstance(m_instance, nullptr);
                m_instance = VK_NULL_HANDLE;
            }

            isInitialized = false;
        }

        IAllocator& m_allocator;
        VkInstance m_instance = VK_NULL_HANDLE;
        VkDebugUtilsMessengerEXT m_debugMessenger = VK_NULL_HANDLE;
        bool m_validationEnabled = false;
        Array<VkAdapterImpl*> m_adapters;
        Array<Adapter*> m_adapterPtrs;

#if defined(__linux__)
        bool m_hasXlib = false;
        bool m_hasWayland = false;
#endif
    };

    /// Creates a Vulkan backend. The caller owns the returned pointer; dispose of it with Destroy().
    [[nodiscard]] Status CreateBackend(const VkBackendDesc& desc, Backend*& out,
                                       IAllocator& allocator = DefaultAllocator())
    {
        out = nullptr;
        auto* b = allocator.New<VkBackendImpl>(allocator);
        if (b == nullptr)
        {
            return ErrorCode::OutOfMemory;
        }
        Status r = b->init(desc.enableValidation);
        if (r != ErrorCode::Ok)
        {
            allocator.Delete(b);
            return r;
        }
        out = b;
        return ErrorCode::Ok;
    }

} // namespace foundation::rhi::vk
