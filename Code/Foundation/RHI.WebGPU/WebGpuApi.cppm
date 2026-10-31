// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// foundation.rhi.webgpu:api - the loaded WebGPU function table.
///
/// The backend never links wgpu at build time. On desktop the implementation is a runtime
/// sidecar (wgpu-native, loaded the same way as DXC): dlopen/LoadLibrary and one dlsym
/// per used function, resolved into this table. On web the browser provides the
/// implementation at link time, so the table is filled with the direct symbol addresses.
/// Backend code always calls through the table, so the two paths are identical above this
/// file.
///
/// Async model: wgpu-native v29 panics on wgpuInstanceWaitAny with a nonzero timeout
/// ("not implemented") even though the header advertises TimedWaitAny. All async
/// completion therefore uses AllowProcessEvents callbacks and a wgpuInstanceProcessEvents
/// pump, never WaitAny.

module;
#include "Core/Prelude.h"
#include "WebGpuIncludes.h"

#if PLATFORM_WEB
#include <emscripten/emscripten.h> // emscripten_sleep: yield to the browser event loop
#include <cstdio>                  // mid-frame-yield diagnostic (fprintf carries a JS stack)
#elif PLATFORM_WINDOWS
#include <windows.h>
#else
#include <dlfcn.h>
#endif

export module foundation.rhi.webgpu:api;

import foundation.core;

using namespace foundation::core;

#if PLATFORM_WEB
// Dawn's webgpu.h (the emdawnwebgpu web port) declares none of the wgpu-native extensions, nor the
// three helper types they need, so decltype(&::fn) on the NATIVE_EXT list below would not compile.
// Forward-declare just enough, at global scope where wgpu-native's wgpu.h puts them on desktop,
// that those members are well-formed function pointers. These symbols are never defined, never
// bound (LoadWebGpuApi binds only the standard list on web) and never invoked (every call site
// null-guards), so no definition or link dependency exists.
using WGPUSubmissionIndex = foundation::core::u64;
struct WGPUInstanceEnumerateAdapterOptions;
using WGPULogCallback = void (*)(int, char const*, void*);
extern "C"
{
    foundation::core::usize wgpuInstanceEnumerateAdapters(WGPUInstance, WGPUInstanceEnumerateAdapterOptions const*, WGPUAdapter*);
    WGPUBool wgpuDevicePoll(WGPUDevice, WGPUBool, WGPUSubmissionIndex const*);
    WGPUSubmissionIndex wgpuQueueSubmitForIndex(WGPUQueue, foundation::core::usize, WGPUCommandBuffer const*);
    float wgpuQueueGetTimestampPeriod(WGPUQueue);
    void wgpuSetLogCallback(WGPULogCallback, void*);
    void wgpuRenderPassEncoderSetImmediates(WGPURenderPassEncoder, foundation::core::u32, void const*, foundation::core::usize);
    void wgpuComputePassEncoderSetImmediates(WGPUComputePassEncoder, foundation::core::u32, void const*, foundation::core::usize);
    void wgpuRenderBundleEncoderSetImmediates(WGPURenderBundleEncoder, foundation::core::u32, void const*, foundation::core::usize);
}
#endif

namespace foundation::rhi::webgpu
{
    // Every standard wgpu entry point the backend uses, present in both the desktop
    // wgpu-native header and Dawn's webgpu.h (the emdawnwebgpu web port). decltype(&::fn)
    // works because webgpu.h declares the functions; nothing references them directly, so
    // no link-time dependency exists. The wgpu-native-only extensions live in the separate
    // NATIVE_EXT list below (absent from Dawn, so compiled out on web).
#define WEBGPU_FUNCTIONS(X)                                                               \
    X(wgpuCreateInstance)                                                                          \
    X(wgpuInstanceRelease)                                                                         \
    X(wgpuInstanceProcessEvents)                                                                   \
    X(wgpuInstanceRequestAdapter)                                                                  \
    X(wgpuAdapterGetInfo)                                                                          \
    X(wgpuAdapterInfoFreeMembers)                                                                  \
    X(wgpuAdapterGetLimits)                                                                        \
    X(wgpuAdapterGetFeatures)                                                                      \
    X(wgpuSupportedFeaturesFreeMembers)                                                            \
    X(wgpuAdapterRequestDevice)                                                                    \
    X(wgpuAdapterRelease)                                                                          \
    X(wgpuDeviceGetQueue)                                                                          \
    X(wgpuDeviceCreateBuffer)                                                                      \
    X(wgpuBufferMapAsync)                                                                          \
    X(wgpuBufferGetConstMappedRange)                                                               \
    X(wgpuBufferUnmap)                                                                             \
    X(wgpuBufferRelease)                                                                           \
    X(wgpuQueueWriteBuffer)                                                                        \
    X(wgpuQueueWriteTexture)                                                                       \
    X(wgpuDeviceCreateTexture)                                                                     \
    X(wgpuTextureRelease)                                                                          \
    X(wgpuTextureGetWidth)                                                                         \
    X(wgpuTextureGetHeight)                                                                        \
    X(wgpuTextureCreateView)                                                                       \
    X(wgpuTextureViewRelease)                                                                      \
    X(wgpuDeviceCreateSampler)                                                                     \
    X(wgpuSamplerRelease)                                                                          \
    X(wgpuDeviceCreateShaderModule)                                                                \
    X(wgpuShaderModuleRelease)                                                                     \
    X(wgpuAdapterHasFeature)                                                                       \
    X(wgpuDeviceCreateBindGroupLayout)                                                             \
    X(wgpuBindGroupLayoutRelease)                                                                  \
    X(wgpuDeviceCreateBindGroup)                                                                   \
    X(wgpuBindGroupRelease)                                                                        \
    X(wgpuDeviceCreatePipelineLayout)                                                              \
    X(wgpuPipelineLayoutRelease)                                                                   \
    X(wgpuDeviceCreateRenderPipeline)                                                              \
    X(wgpuRenderPipelineRelease)                                                                   \
    X(wgpuDeviceCreateComputePipeline)                                                             \
    X(wgpuComputePipelineRelease)                                                                  \
    X(wgpuDeviceCreateCommandEncoder)                                                              \
    X(wgpuCommandEncoderBeginRenderPass)                                                           \
    X(wgpuCommandEncoderBeginComputePass)                                                          \
    X(wgpuCommandEncoderCopyBufferToBuffer)                                                        \
    X(wgpuCommandEncoderCopyBufferToTexture)                                                       \
    X(wgpuCommandEncoderCopyTextureToBuffer)                                                       \
    X(wgpuCommandEncoderCopyTextureToTexture)                                                      \
    X(wgpuCommandEncoderWriteTimestamp)                                                            \
    X(wgpuCommandEncoderResolveQuerySet)                                                           \
    X(wgpuCommandEncoderPushDebugGroup)                                                            \
    X(wgpuCommandEncoderPopDebugGroup)                                                             \
    X(wgpuCommandEncoderInsertDebugMarker)                                                         \
    X(wgpuCommandEncoderFinish)                                                                    \
    X(wgpuCommandEncoderRelease)                                                                   \
    X(wgpuCommandBufferRelease)                                                                    \
    X(wgpuRenderPassEncoderSetPipeline)                                                            \
    X(wgpuRenderPassEncoderSetBindGroup)                                                           \
    X(wgpuRenderPassEncoderSetVertexBuffer)                                                        \
    X(wgpuRenderPassEncoderSetIndexBuffer)                                                         \
    X(wgpuRenderPassEncoderSetViewport)                                                            \
    X(wgpuRenderPassEncoderSetScissorRect)                                                         \
    X(wgpuRenderPassEncoderSetBlendConstant)                                                       \
    X(wgpuRenderPassEncoderSetStencilReference)                                                    \
    X(wgpuRenderPassEncoderDraw)                                                                   \
    X(wgpuRenderPassEncoderDrawIndexed)                                                            \
    X(wgpuRenderPassEncoderDrawIndirect)                                                           \
    X(wgpuRenderPassEncoderDrawIndexedIndirect)                                                    \
    X(wgpuRenderPassEncoderExecuteBundles)                                                         \
    X(wgpuRenderPassEncoderBeginOcclusionQuery)                                                    \
    X(wgpuRenderPassEncoderEndOcclusionQuery)                                                      \
    X(wgpuRenderPassEncoderEnd)                                                                    \
    X(wgpuRenderPassEncoderRelease)                                                                \
    X(wgpuComputePassEncoderSetPipeline)                                                           \
    X(wgpuComputePassEncoderSetBindGroup)                                                          \
    X(wgpuComputePassEncoderDispatchWorkgroups)                                                    \
    X(wgpuComputePassEncoderDispatchWorkgroupsIndirect)                                            \
    X(wgpuComputePassEncoderEnd)                                                                   \
    X(wgpuComputePassEncoderRelease)                                                               \
    X(wgpuDeviceCreateRenderBundleEncoder)                                                         \
    X(wgpuRenderBundleEncoderSetPipeline)                                                          \
    X(wgpuRenderBundleEncoderSetBindGroup)                                                         \
    X(wgpuRenderBundleEncoderSetVertexBuffer)                                                      \
    X(wgpuRenderBundleEncoderSetIndexBuffer)                                                       \
    X(wgpuRenderBundleEncoderDraw)                                                                 \
    X(wgpuRenderBundleEncoderDrawIndexed)                                                          \
    X(wgpuRenderBundleEncoderDrawIndirect)                                                         \
    X(wgpuRenderBundleEncoderDrawIndexedIndirect)                                                  \
    X(wgpuRenderBundleEncoderFinish)                                                               \
    X(wgpuRenderBundleEncoderRelease)                                                              \
    X(wgpuRenderBundleRelease)                                                                     \
    X(wgpuDeviceCreateQuerySet)                                                                    \
    X(wgpuQuerySetRelease)                                                                         \
    X(wgpuInstanceCreateSurface)                                                                   \
    X(wgpuSurfaceConfigure)                                                                        \
    X(wgpuSurfaceGetCapabilities)                                                                  \
    X(wgpuSurfaceCapabilitiesFreeMembers)                                                          \
    X(wgpuSurfaceUnconfigure)                                                                      \
    X(wgpuSurfaceGetCurrentTexture)                                                                \
    X(wgpuSurfacePresent)                                                                          \
    X(wgpuSurfaceRelease)                                                                          \
    X(wgpuDeviceRelease)                                                                           \
    X(wgpuQueueSubmit)                                                                             \
    X(wgpuQueueOnSubmittedWorkDone)                                                                \
    X(wgpuQueueRelease)

    // wgpu-native-only entry points. Present in wgpu-native's wgpu.h (desktop), absent from
    // Dawn's webgpu.h (the emdawnwebgpu web port). On desktop they resolve like any other;
    // on web they stay null and every call site null-guards (SetImmediates falls back
    // below).
#define WEBGPU_NATIVE_EXT_FUNCTIONS(X)                                                    \
    X(wgpuInstanceEnumerateAdapters)                                                               \
    X(wgpuDevicePoll)                                                                              \
    X(wgpuQueueSubmitForIndex)                                                                     \
    X(wgpuQueueGetTimestampPeriod)                                                                 \
    X(wgpuSetLogCallback)                                                                          \
    X(wgpuRenderPassEncoderSetImmediates)                                                          \
    X(wgpuComputePassEncoderSetImmediates)                                                         \
    X(wgpuRenderBundleEncoderSetImmediates)

    export struct WebGpuApi
    {
#define WEBGPU_DECLARE_MEMBER(fn) decltype(&::fn) fn = nullptr;
        WEBGPU_FUNCTIONS(WEBGPU_DECLARE_MEMBER)
        WEBGPU_NATIVE_EXT_FUNCTIONS(WEBGPU_DECLARE_MEMBER)
#undef WEBGPU_DECLARE_MEMBER

        void* libraryHandle = nullptr; // desktop sidecar handle; null on web
        // Whether the instance was created with ShaderSourceSPIRV (the standard
        // instance feature): the desktop DXC dev loop. Browsers never have it.
        bool spirvIngestion = false;

        /// Return control to the browser event loop so its microtasks run. This is the only
        /// way a WebGPU completion future (adapter/device/map/work-done) resolves on web,
        /// since those callbacks fire from a microtask that cannot run while wasm spins.
        /// Requires the linking executable to enable ASYNCIFY. A no-op on desktop, where
        /// wgpu-native delivers callbacks synchronously from ProcessEvents/DevicePoll, so
        /// every pump loop below is unchanged there and only gains a real progress path on
        /// web.
        ///
        /// Not named Yield: <windows.h> defines Yield() as an empty function-like macro, so
        /// that spelling breaks the declaration and silently erases every call site.
        void YieldToEventLoop() const
        {
#if PLATFORM_WEB
            // The canvas texture the swapchain acquired expires when the requestAnimationFrame
            // callback returns (the browser destroys it at composite time). Yielding between
            // AcquireNextImage and Present hands control back to the browser mid-frame, so this
            // frame's later submit lands on a destroyed texture ("Destroyed texture used in a
            // submit") and Dawn drops the whole command buffer, silently losing any one-shot
            // work it carried (env/probe bakes, the ImGui font atlas). Log it as an error; the
            // browser console attaches the full call stack to this line, naming the waiter.
            if (frameOpen && !frameYieldWarned)
            {
                frameYieldWarned = true;
                // EM_LOG_C_STACK prints the wasm call stack inline (no console expansion
                // needed); the frames name the mid-frame waiter to fix.
                emscripten_log(EM_LOG_ERROR | EM_LOG_C_STACK,
                               "[webgpu] YieldToEventLoop DURING an open frame - the browser "
                               "will expire the canvas texture and this frame's submit will "
                               "be dropped; the stack below is the mid-frame waiter:");
            }
            emscripten_sleep(0);
#endif
        }

        // Web frame-lifetime tracking for the diagnostic above; the swapchain marks the frame
        // open at a successful AcquireNextImage and closed at Present. Desktop never sets it.
        void NoteFrameOpen() const noexcept
        {
            frameOpen = true;
            frameYieldWarned = false;
        }
        void NoteFrameClosed() const noexcept { frameOpen = false; }
        mutable bool frameOpen = false;
        mutable bool frameYieldWarned = false;

        /// Pumps callback delivery for AllowProcessEvents-mode futures until `done`
        /// flips or the iteration guard trips. Suits creation-time waits (nothing on
        /// the GPU timeline); GPU-completion waits use PumpUntilWithDevice.
        void PumpUntil(WGPUInstance instance, const bool& done) const
        {
            for (u32 i = 0; i < 100000 && !done; ++i)
            {
                wgpuInstanceProcessEvents(instance);
                YieldToEventLoop();
            }
        }

        /// GPU-completion pump: map/work-done callbacks fire only when the device is
        /// polled, and a bare ProcessEvents spin can starve under real frame load.
        /// DevicePoll is non-blocking (wait=0): wait=1 can block forever when the queue
        /// is already empty (e.g. a MapAsync pending with no submissions in flight). On
        /// web the browser's event loop delivers; ProcessEvents suffices.
        void PumpUntilWithDevice(WGPUInstance instance, WGPUDevice device,
                                 const bool& done) const
        {
            for (u32 i = 0; i < 1000000 && !done; ++i)
            {
                wgpuInstanceProcessEvents(instance);
                if (done)
                {
                    return;
                }
                if (wgpuDevicePoll != nullptr && device != nullptr)
                {
                    (void)wgpuDevicePoll(device, 0u, nullptr);
                }
                YieldToEventLoop(); // web: the only progress path (DevicePoll is null there)
            }
        }
    };

    /// Fills `api`. Desktop: dlopen the wgpu-native sidecar (explicit path override, else
    /// the vendored BUILDSYSTEM_WGPU_PATH, else the bare soname so a relocated dist
    /// resolves the copy staged beside the executable via $ORIGIN). Web: direct symbols.
    export Status LoadWebGpuApi(WebGpuApi& api, StringView libraryPathOverride);

    /// Releases the sidecar handle (a no-op on web or when never loaded).
    export void UnloadWebGpuApi(WebGpuApi& api);

#if PLATFORM_WEB

    Status LoadWebGpuApi(WebGpuApi& api, StringView)
    {
        // The browser (via emscripten's library_webgpu) is the implementation; the
        // wgpu-native-extension entries stay null and must not be reached on web.
#define WEBGPU_BIND_DIRECT(fn) api.fn = &::fn;
        WEBGPU_FUNCTIONS(WEBGPU_BIND_DIRECT)
#undef WEBGPU_BIND_DIRECT
        return ErrorCode::Ok;
    }

    void UnloadWebGpuApi(WebGpuApi&) {}

#else

    Status LoadWebGpuApi(WebGpuApi& api, StringView libraryPathOverride)
    {
#if PLATFORM_WINDOWS
        HMODULE lib = nullptr;
        if (!libraryPathOverride.IsEmpty())
        {
            String path(libraryPathOverride);
            lib = LoadLibraryA(reinterpret_cast<const char*>(path.CStr()));
        }
#ifdef BUILDSYSTEM_WGPU_PATH
        if (lib == nullptr)
        {
            lib = LoadLibraryA(BUILDSYSTEM_WGPU_PATH);
        }
#endif
        if (lib == nullptr)
        {
            lib = LoadLibraryA("wgpu_native.dll"); // dist: staged beside the executable
        }
        if (lib == nullptr)
        {
            return ErrorCode::NotFound;
        }
        const auto resolve = [lib](const char* name) -> void*
        { return reinterpret_cast<void*>(GetProcAddress(lib, name)); };
#else
        void* lib = nullptr;
        if (!libraryPathOverride.IsEmpty())
        {
            String path(libraryPathOverride);
            lib = dlopen(reinterpret_cast<const char*>(path.CStr()), RTLD_NOW | RTLD_LOCAL);
        }
#ifdef BUILDSYSTEM_WGPU_PATH
        if (lib == nullptr)
        {
            lib = dlopen(BUILDSYSTEM_WGPU_PATH, RTLD_NOW | RTLD_LOCAL);
        }
#endif
        if (lib == nullptr)
        {
            // Relocated dist: the vendored absolute path does not exist there, so the bare
            // soname searches RUNPATH ($ORIGIN, the copy staged beside the executable).
            lib = dlopen("libwgpu_native.so", RTLD_NOW | RTLD_LOCAL);
        }
        if (lib == nullptr)
        {
            return ErrorCode::NotFound;
        }
        const auto resolve = [lib](const char* name) -> void* { return dlsym(lib, name); };
#endif

        bool allResolved = true;
#define WEBGPU_RESOLVE(fn)                                                                \
    api.fn = reinterpret_cast<decltype(&::fn)>(resolve(#fn));                                      \
    allResolved = allResolved && api.fn != nullptr;
        WEBGPU_FUNCTIONS(WEBGPU_RESOLVE)
        WEBGPU_NATIVE_EXT_FUNCTIONS(WEBGPU_RESOLVE)
#undef WEBGPU_RESOLVE

        if (!allResolved)
        {
            api.libraryHandle = lib;
            UnloadWebGpuApi(api);
            return ErrorCode::NotSupported; // wrong or too-old sidecar
        }
        api.libraryHandle = lib;
        return ErrorCode::Ok;
    }

    void UnloadWebGpuApi(WebGpuApi& api)
    {
        if (api.libraryHandle == nullptr)
        {
            return;
        }
#if PLATFORM_WINDOWS
        FreeLibrary(static_cast<HMODULE>(api.libraryHandle));
#else
        dlclose(api.libraryHandle);
#endif
        api.libraryHandle = nullptr;
    }

#endif // PLATFORM_WEB
}
