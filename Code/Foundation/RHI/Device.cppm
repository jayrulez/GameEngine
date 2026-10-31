// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Abstract Backend, Adapter, and Device interfaces.
///
/// Backend is the entry point: it enumerates GPU adapters and creates
/// presentation surfaces. Adapter represents a physical GPU. Device is
/// the central factory for all GPU resources.
///
/// Mesh shader and ray tracing creation methods are on Device directly
/// (virtual no-ops returning ErrorCode::NotSupported when not supported).
/// Callers check device->features.meshShaders or .rayTracing before use.

module;

export module foundation.rhi:device;

import foundation.core;
import :enums;
import :texture_format;
import :types;
import :descriptors;
import :counters;
import :ext_descriptors;
import :resources;
import :commands;
import :queue;
import :swapchain;

using namespace foundation::core;

export namespace foundation::rhi
{

    // ---- Backend ----

    /// The windowing system that produced a native window's handles. Passed to CreateSurface so the backend
    /// creates the matching platform surface (feeding an X11 Display* to the Wayland WSI segfaults).
    /// The neutral RHI counterpart of shell::WindowSystem (the RHI must not depend on the shell); the
    /// graphics and sample layers map one to the other. With Unknown the backend falls back to its
    /// best guess.
    enum class SurfacePlatform
    {
        Unknown,
        Win32,
        X11,
        Wayland,
        Cocoa
    };

    /// RHI backend entry point (Vulkan, DX12, etc.).
    class Backend
    {
    public:
        virtual ~Backend() = default;

        bool isInitialized = false;

        /// Returns all available GPU adapters in preference order, most
        /// preferred first (discrete, then integrated, then unknown, then CPU). Backends
        /// guarantee this ordering through SortAdaptersByPreference(), so callers
        /// that just want "the best available GPU" can take element [0].
        [[nodiscard]] virtual Span<Adapter* const> EnumerateAdapters() = 0;

        /// Creates a presentation surface from a native window handle. `platform` tells the backend which WSI
        /// produced the handles so it selects the matching surface type instead of guessing (Unknown guesses).
        /// On Win32: windowHandle = HWND, displayHandle = nullptr.
        /// On X11: windowHandle = XID (as void*), displayHandle = Display*.
        /// On Wayland: windowHandle = wl_surface*, displayHandle = wl_display*.
        virtual Status CreateSurface(void* windowHandle, void* displayHandle, Surface*& out,
                                     SurfacePlatform platform = SurfacePlatform::Unknown) = 0;

        Status CreateSurface(void* windowHandle, Surface*& out)
        {
            return CreateSurface(windowHandle, nullptr, out, SurfacePlatform::Unknown);
        }

        /// Destroy the backend and all objects it owns.
        virtual void Destroy() = 0;
    };

    // ---- Adapter ----

    /// Represents a physical GPU. Query capabilities and create a logical device.
    class Adapter
    {
    public:
        virtual ~Adapter() = default;

        /// Populate adapter info (name, vendor, features, limits).
        virtual void GetInfo(AdapterInfo& out) = 0;

        /// Convenience: returns a copy of the adapter info.
        [[nodiscard]] AdapterInfo Info()
        {
            AdapterInfo i;
            GetInfo(i);
            return i;
        }

        /// Create a logical device from this adapter.
        virtual Status CreateDevice(const DeviceDesc& desc, Device*& out) = 0;
    };

    /// Selection preference for an adapter type; lower is more preferred.
    /// Defines the single source of truth for "best GPU first" ordering.
    [[nodiscard]] inline int AdapterPreferenceRank(AdapterType type)
    {
        switch (type)
        {
        case AdapterType::DiscreteGpu:
            return 0;
        case AdapterType::IntegratedGpu:
            return 1;
        case AdapterType::Unknown:
            return 2;
        case AdapterType::Cpu:
            return 3;
        }
        return 4;
    }

    /// Reorder adapters so the most preferred GPU is first (see
    /// AdapterPreferenceRank). Backends call this after enumeration so that
    /// EnumerateAdapters()[0] is the recommended default. The sort is stable,
    /// preserving the driver's native order among adapters of equal type.
    inline void SortAdaptersByPreference(Array<Adapter*>& adapters)
    {
        // Stable insertion sort by preference rank (adapter counts are tiny).
        for (usize i = 1; i < adapters.Size(); ++i)
        {
            Adapter* key = adapters[i];
            const int keyRank = AdapterPreferenceRank(key->Info().type);
            usize j = i;
            while (j > 0 && AdapterPreferenceRank(adapters[j - 1]->Info().type) > keyRank)
            {
                adapters[j] = adapters[j - 1];
                --j;
            }
            adapters[j] = key;
        }
    }

    // ---- Device ----

    /// Central factory for GPU resources, pipelines, and command infrastructure.
    class Device
    {
    public:
        virtual ~Device() = default;

        DeviceType type = DeviceType::Null;
        DeviceFeatures features{};

        // The shader format this device's CreateShaderModule expects: SPIR-V (Vulkan, Null, native
        // wgpu-native WebGPU), DXIL (DX12), or WGSL text (browser WebGPU). The shader cook/system pick
        // the cooked-blob format from this.
        [[nodiscard]] virtual ShaderFormat PreferredShaderFormat() const noexcept = 0;

        // True when this backend does not flip clip-space Y in its viewport. Vulkan and DX12 use a
        // negative-height viewport (which also inverts front-face winding); WebGPU's setViewport
        // cannot take a negative height, so it gets no such flip. Screen-space reconstruction passes
        // (sky ray, shadows, SSAO/SSR) that unproject NDC via invViewProj must negate the NDC Y (or,
        // equivalently, flip the uploaded invViewProj's Y row) when this is true.
        [[nodiscard]] virtual bool NeedsClipSpaceYFlip() const noexcept = 0;

        // ---- Queries ----
        [[nodiscard]] virtual Queue* GetQueue(QueueType type, u32 index = 0) = 0;
        [[nodiscard]] virtual u32 GetQueueCount(QueueType type) = 0;
        /// Query hardware format support for a given texture format.
        [[nodiscard]] virtual FormatSupport GetFormatSupport(TextureFormat format) = 0;
        /// The maximum MSAA sample count supported for both color and depth framebuffer attachments
        /// (scene-pass MSAA). The engine caps it at 4 (8x is not supported).
        /// The default of 1 means a backend that does not implement the query reports no MSAA,
        /// which is safe: views clamp to single-sample. Backends that support MSAA override it.
        [[nodiscard]] virtual u32 MaxColorDepthSampleCount() const noexcept { return 1; }
        /// Whether this sample count is usable for both color and depth render targets. The set of
        /// valid counts is not simply [1 .. MaxColorDepthSampleCount()]: WebGPU, for instance,
        /// guarantees only {1, 4} and never 2, so a caller must snap an unsupported count rather than
        /// assume every count up to the ceiling works (an unsupported count aborts texture or pipeline
        /// creation). The default accepts only 1 (no MSAA); backends that support MSAA override it.
        [[nodiscard]] virtual bool SupportsSampleCount(u32 count) const noexcept { return count <= 1; }

        /// What this device has recorded since it was made and the memory it holds now (the
        /// profiler's counters; DeviceCounters).
        [[nodiscard]] virtual DeviceCounters Counters() const noexcept = 0;
        /// The driver's own GPU memory figures, where it gives them (Features::memoryBudget):
        /// what the device-local heaps may hold and what this process uses of them. False, both
        /// left alone, when it does not.
        [[nodiscard]] virtual bool MemoryBudget(u64& budgetBytes, u64& usedBytes) const = 0;

        /// The bytes one query of `type` takes in the buffer ResolveQuerySet writes: 8 for a
        /// timestamp or occlusion count, a backend's own record for pipeline statistics (0 when
        /// the device has none, Features::pipelineStatisticsQueries).
        [[nodiscard]] virtual u32 QueryResultSize(QueryType queryType) const noexcept = 0;
        /// One resolved pipeline-statistics query (QueryResultSize(PipelineStatistics) bytes at
        /// `result`) in the RHI's terms; all zero without the feature.
        [[nodiscard]] virtual PipelineStatistics DecodePipelineStatistics(const void* result) const noexcept = 0;

        // ---- Resource creation ----
        virtual Status CreateBuffer(const BufferDesc& desc, Buffer*& out) = 0;
        virtual Status CreateTexture(const TextureDesc& desc, Texture*& out) = 0;
        virtual Status CreateTextureView(Texture* texture, const TextureViewDesc& desc,
                                         TextureView*& out) = 0;
        virtual Status CreateSampler(const SamplerDesc& desc, Sampler*& out) = 0;
        virtual Status CreateShaderModule(const ShaderModuleDesc& desc, ShaderModule*& out) = 0;
        virtual Status CreateBindGroupLayout(const BindGroupLayoutDesc& desc,
                                             BindGroupLayout*& out) = 0;
        virtual Status CreateBindGroup(const BindGroupDesc& desc, BindGroup*& out) = 0;
        virtual Status CreatePipelineLayout(const PipelineLayoutDesc& desc,
                                            PipelineLayout*& out) = 0;
        virtual Status CreatePipelineCache(const PipelineCacheDesc& desc, PipelineCache*& out) = 0;
        virtual Status CreateRenderPipeline(const RenderPipelineDesc& desc,
                                            RenderPipeline*& out) = 0;
        virtual Status CreateComputePipeline(const ComputePipelineDesc& desc,
                                             ComputePipeline*& out) = 0;
        virtual Status CreateCommandPool(QueueType queueType, CommandPool*& out) = 0;
        virtual Status CreateFence(u64 initialValue, Fence*& out) = 0;
        virtual Status CreateQuerySet(const QuerySetDesc& desc, QuerySet*& out) = 0;
        virtual Status CreateSwapChain(Surface* surface, const SwapChainDesc& desc,
                                       SwapChain*& out) = 0;

        // ---- Resource destruction ----
        virtual void DestroyBuffer(Buffer*& buf) = 0;
        virtual void DestroyTexture(Texture*& tex) = 0;
        virtual void DestroyTextureView(TextureView*& view) = 0;
        virtual void DestroySampler(Sampler*& sampler) = 0;
        virtual void DestroyShaderModule(ShaderModule*& module) = 0;
        virtual void DestroyBindGroupLayout(BindGroupLayout*& layout) = 0;
        virtual void DestroyBindGroup(BindGroup*& group) = 0;
        virtual void DestroyPipelineLayout(PipelineLayout*& layout) = 0;
        virtual void DestroyPipelineCache(PipelineCache*& cache) = 0;
        virtual void DestroyRenderPipeline(RenderPipeline*& pipeline) = 0;
        virtual void DestroyComputePipeline(ComputePipeline*& pipeline) = 0;
        virtual void DestroyCommandPool(CommandPool*& pool) = 0;
        virtual void DestroyFence(Fence*& fence) = 0;
        virtual void DestroyQuerySet(QuerySet*& querySet) = 0;
        virtual void DestroySwapChain(SwapChain*& swapChain) = 0;
        virtual void DestroySurface(Surface*& surface) = 0;

        // ---- Mesh shader extension (folded into Device) ----
        /// Create a mesh shader pipeline. Returns NotSupported if mesh shaders
        /// are not enabled on this device.
        virtual Status CreateMeshPipeline(const MeshPipelineDesc& desc, MeshPipeline*& out)
        {
            (void)desc;
            out = nullptr;
            return ErrorCode::NotSupported;
        }
        virtual void DestroyMeshPipeline(MeshPipeline*& pipeline) { (void)pipeline; }

        // ---- Ray tracing extension (folded into Device) ----
        /// Shader binding table handle properties. Populated by the backend
        /// during device creation when ray tracing is enabled.
        u32 shaderGroupHandleSize = 0;
        u32 shaderGroupHandleAlignment = 0;
        u32 shaderGroupBaseAlignment = 0;

        /// Create an acceleration structure. Returns NotSupported if ray tracing
        /// is not enabled on this device.
        virtual Status CreateAccelStruct(const AccelStructDesc& desc, AccelStruct*& out)
        {
            (void)desc;
            out = nullptr;
            return ErrorCode::NotSupported;
        }
        virtual void DestroyAccelStruct(AccelStruct*& accelStruct) { (void)accelStruct; }

        virtual Status CreateRayTracingPipeline(const RayTracingPipelineDesc& desc,
                                                RayTracingPipeline*& out)
        {
            (void)desc;
            out = nullptr;
            return ErrorCode::NotSupported;
        }
        virtual void DestroyRayTracingPipeline(RayTracingPipeline*& pipeline) { (void)pipeline; }

        /// Retrieve shader group handles for building shader binding tables.
        virtual Status GetShaderGroupHandles(RayTracingPipeline* pipeline, u32 firstGroup,
                                             u32 groupCount, Span<u8> outData)
        {
            (void)pipeline;
            (void)firstGroup;
            (void)groupCount;
            (void)outData;
            return ErrorCode::NotSupported;
        }

        // ---- Lifecycle ----
        /// Whether the device has been lost (GPU hang/TDR, driver reset, removal).
        /// Sticky: once lost, the device cannot recover; stop submitting work and
        /// destroy and recreate it. DX12 queries the device directly; Vulkan latches
        /// VK_ERROR_DEVICE_LOST from queue submit, present and wait results.
        [[nodiscard]] virtual bool IsLost() = 0;
        virtual void WaitIdle() = 0;
        virtual void Destroy() = 0;
    };

} // namespace foundation::rhi
