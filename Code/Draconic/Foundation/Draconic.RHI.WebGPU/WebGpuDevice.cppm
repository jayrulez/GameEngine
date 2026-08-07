/// draconic.rhi.webgpu:device - Device over WGPUDevice.
///
/// BRING-UP STAGE: real device/queue lifecycle (creation, loss latch, WaitIdle,
/// fences, destruction); every resource/pipeline/command factory is an HONEST
/// NotSupported until its stage lands - callers get failures, never silent fakes.
/// Build-out order (web-platform.md P1): resources -> pipelines/bind groups ->
/// encoders + swapchain (triangle) -> transfer/queries/bundles (full renderer).

module;
#include "Draconic.Foundation/Prelude.h"
#include "WebGpuIncludes.h"

export module draconic.rhi.webgpu:device;

import draconic.foundation;
import draconic.rhi;
import :api;
import :blit_helper;
import :buffer;
import :texture;
import :texture_view;
import :sampler;
import :shader_module;
import :bind_group_layout;
import :bind_group;
import :pipeline_layout;
import :pipeline_cache;
import :render_pipeline;
import :compute_pipeline;
import :query_set;
import :command_pool;
import :surface;
import :swapchain;
import :fence;
import :queue;

using namespace draconic::foundation;

export namespace draconic::rhi::webgpu
{
    class WebGpuDevice final : public Device
    {
    public:
        WebGpuDevice(const WebGpuApi& api, WGPUInstance instance, WGPUAdapter adapter,
                     WGPUDevice device, IAllocator& allocator)
            : m_api(&api), m_instance(instance), m_adapter(adapter), m_device(device),
              m_allocator(allocator)
        {
            type = DeviceType::WebGPU;
            const WGPUQueue queue = m_api->wgpuDeviceGetQueue(m_device);
            // ONE WebGPU queue, three RHI-typed views of it (see :queue).
            m_graphicsQueue.Initialize(api, instance, device, queue, allocator,
                                       QueueType::Graphics, m_bufferRegistry);
            m_computeQueue.Initialize(api, instance, device, queue, allocator,
                                      QueueType::Compute, m_bufferRegistry);
            m_transferQueue.Initialize(api, instance, device, queue, allocator,
                                       QueueType::Transfer, m_bufferRegistry);
            m_blitHelper.Initialize(api, device);
            // DEBUG: force the FULL browser shader path on desktop wgpu-native (see
            // PreferredShaderFormat) - WGSL text AND push-constant emulation. The WGSL cook always
            // emulates push constants (browsers reject var<push_constant>), so the layout must emulate
            // too or the emulated @group(N) @binding(0) block mismatches a native-immediate layout.
            m_forceWgsl = GetEnvironmentVariable(u8"DRACONIC_WEBGPU_WGSL").HasValue();
            if (m_forceWgsl)
            {
                m_forceUniformPushConstants = true;
            }
        }

        [[nodiscard]] WebGpuBlitHelper& BlitHelper() { return m_blitHelper; }

        /// The device-lost callback (registered at creation by the adapter) lands here.
        void MarkLost() { m_lost = true; }

        /// Set by the adapter: whether the device carries the Immediates feature
        /// (push-constant support; pipeline layouts with ranges fail without it).
        void SetImmediatesSupported(bool supported) { m_immediatesSupported = supported; }

        /// Force the uniform-buffer push-constant fallback even where immediates exist.
        /// Web always emulates (no immediates in Dawn); this lets desktop tests exercise the
        /// same path against a real GPU. See WebGpuPipelineLayout / the pass encoders.
        void SetForceUniformPushConstants(bool force) { m_forceUniformPushConstants = force; }

        /// Whether push constants are emulated as a bound uniform buffer on this device
        /// (no immediates, or the fallback forced) rather than issued via SetImmediates.
        [[nodiscard]] bool EmulatesPushConstants() const
        {
            return m_forceUniformPushConstants || !m_immediatesSupported;
        }

        [[nodiscard]] WGPUDevice Handle() const { return m_device; }
        [[nodiscard]] WGPUInstance Instance() const { return m_instance; }
        [[nodiscard]] const WebGpuApi& Api() const { return *m_api; }

        // SPIR-V ingestion is a native wgpu-native feature (ShaderSourceSPIRV instance feature); a
        // browser never exposes it, so there the cook must feed WGSL text instead.
        // DEBUG override: DRACONIC_WEBGPU_WGSL forces the WGSL path on desktop wgpu-native, so the
        // desktop --webgpu run exercises the EXACT browser shaders (WGSL/naga-frontend) instead of
        // SPIR-V ingestion - a faithful, fast repro for web-render bugs. Requires a WGSL shaders.dpak.
        [[nodiscard]] ShaderFormat PreferredShaderFormat() const noexcept override
        {
            if (m_forceWgsl)
            {
                return ShaderFormat::WGSL;
            }
            return m_api->spirvIngestion ? ShaderFormat::SpirV : ShaderFormat::WGSL;
        }

        // False: WebGPU raster orientation matches Vulkan's for BOTH shader paths, probe-proven in
        // Draconic.Render.Backend.Tests. wgpu's runtime SPIR-V frontend applies no clip-space
        // adjustment, and the WGSL cook passes naga --keep-coordinate-space so the cooked WGSL
        // carries the same convention. No renderer pass needs a Y compensation on this backend.
        [[nodiscard]] bool NeedsClipSpaceYFlip() const noexcept override { return false; }

        // ---- Queries ----
        Queue* GetQueue(QueueType queueType, u32 index) override
        {
            if (index != 0)
            {
                return nullptr;
            }
            switch (queueType)
            {
            case QueueType::Graphics:
                return &m_graphicsQueue;
            case QueueType::Compute:
                return &m_computeQueue;
            case QueueType::Transfer:
                return &m_transferQueue;
            }
            return nullptr;
        }

        u32 GetQueueCount(QueueType) override { return 1; }

        FormatSupport GetFormatSupport(TextureFormat format) override
        {
            // WebGPU's per-format capabilities are SPEC tables, not driver queries -
            // encode the classes the renderer asks about.
            switch (format)
            {
            case TextureFormat::Undefined:
                return FormatSupport::Unsupported;

            // Depth/stencil family.
            case TextureFormat::Depth16Unorm:
            case TextureFormat::Depth24Plus:
            case TextureFormat::Depth24PlusStencil8:
            case TextureFormat::Depth32Float:
            case TextureFormat::Depth32FloatStencil8:
            case TextureFormat::Stencil8:
                return FormatSupport::Texture | FormatSupport::DepthStencil;

            // BC block formats: sampled-only, feature-gated.
            case TextureFormat::BC1RGBAUnorm:
            case TextureFormat::BC1RGBAUnormSrgb:
            case TextureFormat::BC2RGBAUnorm:
            case TextureFormat::BC2RGBAUnormSrgb:
            case TextureFormat::BC3RGBAUnorm:
            case TextureFormat::BC3RGBAUnormSrgb:
            case TextureFormat::BC4RUnorm:
            case TextureFormat::BC4RSnorm:
            case TextureFormat::BC5RGUnorm:
            case TextureFormat::BC5RGSnorm:
            case TextureFormat::BC6HRGBUfloat:
            case TextureFormat::BC6HRGBFloat:
            case TextureFormat::BC7RGBAUnorm:
            case TextureFormat::BC7RGBAUnormSrgb:
                return features.textureCompressionBC
                           ? FormatSupport::Texture | FormatSupport::LinearFilter
                           : FormatSupport::Unsupported;

            // 16-bit norm formats have no core WebGPU equivalent at all.
            case TextureFormat::RGBA16Unorm:
            case TextureFormat::RGBA16Snorm:
                return FormatSupport::Unsupported;

            // 32-bit float family: renderable + storage, NOT filterable (core).
            case TextureFormat::R32Float:
            case TextureFormat::RG32Float:
            case TextureFormat::RGBA32Float:
                return FormatSupport::Texture | FormatSupport::ColorAttachment |
                       FormatSupport::StorageTexture;

            // Integer formats: renderable, never blendable or filterable.
            case TextureFormat::R8Uint:
            case TextureFormat::R8Sint:
            case TextureFormat::R16Uint:
            case TextureFormat::R16Sint:
            case TextureFormat::R32Uint:
            case TextureFormat::R32Sint:
            case TextureFormat::RG8Uint:
            case TextureFormat::RG8Sint:
            case TextureFormat::RG16Uint:
            case TextureFormat::RG16Sint:
            case TextureFormat::RG32Uint:
            case TextureFormat::RG32Sint:
            case TextureFormat::RGBA8Uint:
            case TextureFormat::RGBA8Sint:
            case TextureFormat::RGBA16Uint:
            case TextureFormat::RGBA16Sint:
            case TextureFormat::RGBA32Uint:
            case TextureFormat::RGBA32Sint:
            case TextureFormat::RGB10A2Uint:
                return FormatSupport::Texture | FormatSupport::ColorAttachment;

            // Snorm + shared-exponent + packed-float oddballs: sampled/filterable;
            // RG11B10 additionally renderable via the RG11B10UfloatRenderable
            // feature on some runtimes - conservatively sampled-only here.
            case TextureFormat::R8Snorm:
            case TextureFormat::RG8Snorm:
            case TextureFormat::RGBA8Snorm:
            case TextureFormat::RGB9E5Float:
            case TextureFormat::RG11B10Float:
                return FormatSupport::Texture | FormatSupport::LinearFilter;

            // Everything else in the RHI list is the classic filterable +
            // renderable + blendable color family.
            default:
                return FormatSupport::Texture | FormatSupport::ColorAttachment |
                       FormatSupport::BlendableColor | FormatSupport::LinearFilter;
            }
        }

        // ---- Resource creation (encoders/pipelines still staged - see class comment) ----
        Status CreateBuffer(const BufferDesc& bufferDesc, Buffer*& out) override
        {
            const Status status = CreateResource<WebGpuBuffer>(
                out, [&](WebGpuBuffer& b)
                { return b.Initialize(*m_api, m_instance, m_device,
                                      m_graphicsQueue.Handle(), bufferDesc); });
            if (status.IsOk())
            {
                m_bufferRegistry.Add(static_cast<WebGpuBuffer*>(out));
            }
            return status;
        }
        Status CreateTexture(const TextureDesc& textureDesc, Texture*& out) override
        {
            return CreateResource<WebGpuTexture>(
                out, [&](WebGpuTexture& t) { return t.Initialize(*m_api, m_device, textureDesc); });
        }
        Status CreateTextureView(Texture* texture, const TextureViewDesc& viewDesc,
                                 TextureView*& out) override
        {
            if (texture == nullptr)
            {
                out = nullptr;
                return ErrorCode::InvalidArgument;
            }
            return CreateResource<WebGpuTextureView>(
                out, [&](WebGpuTextureView& v) { return v.Initialize(*m_api, texture, viewDesc); });
        }
        Status CreateSampler(const SamplerDesc& samplerDesc, Sampler*& out) override
        {
            return CreateResource<WebGpuSampler>(
                out, [&](WebGpuSampler& smp) { return smp.Initialize(*m_api, m_device, samplerDesc); });
        }
        Status CreateShaderModule(const ShaderModuleDesc& moduleDesc, ShaderModule*& out) override
        {
            return CreateResource<WebGpuShaderModule>(
                out, [&](WebGpuShaderModule& m) { return m.Initialize(*m_api, m_device, moduleDesc); });
        }
        Status CreateBindGroupLayout(const BindGroupLayoutDesc& layoutDesc,
                                     BindGroupLayout*& out) override
        {
            return CreateResource<WebGpuBindGroupLayout>(
                out, [&](WebGpuBindGroupLayout& l)
                { return l.Initialize(*m_api, m_device, layoutDesc); });
        }
        Status CreateBindGroup(const BindGroupDesc& groupDesc, BindGroup*& out) override
        {
            return CreateResource<WebGpuBindGroup>(
                out,
                [&](WebGpuBindGroup& g) { return g.Initialize(*m_api, m_device, groupDesc); });
        }
        Status CreatePipelineLayout(const PipelineLayoutDesc& layoutDesc,
                                    PipelineLayout*& out) override
        {
            return CreateResource<WebGpuPipelineLayout>(
                out, [&](WebGpuPipelineLayout& l)
                { return l.Initialize(*m_api, m_device, layoutDesc, EmulatesPushConstants()); });
        }
        Status CreatePipelineCache(const PipelineCacheDesc&, PipelineCache*& out) override
        {
            // No cache object in WebGPU; a benign empty stand-in keeps callers happy.
            out = m_allocator.New<WebGpuPipelineCache>();
            return ErrorCode::Ok;
        }
        Status CreateRenderPipeline(const RenderPipelineDesc& pipelineDesc,
                                    RenderPipeline*& out) override
        {
            return CreateResource<WebGpuRenderPipeline>(
                out, [&](WebGpuRenderPipeline& p)
                { return p.Initialize(*m_api, m_device, pipelineDesc); });
        }
        Status CreateComputePipeline(const ComputePipelineDesc& pipelineDesc,
                                     ComputePipeline*& out) override
        {
            return CreateResource<WebGpuComputePipeline>(
                out, [&](WebGpuComputePipeline& p)
                { return p.Initialize(*m_api, m_device, pipelineDesc); });
        }
        Status CreateCommandPool(QueueType, CommandPool*& out) override
        {
            // All queue types funnel into the ONE WebGPU queue; pools are bookkeeping.
            auto* pool = m_allocator.New<WebGpuCommandPool>();
            pool->Initialize(*m_api, m_device, m_allocator, m_blitHelper);
            out = pool;
            return ErrorCode::Ok;
        }
        Status CreateFence(u64 initialValue, Fence*& out) override
        {
            out = m_allocator.New<WebGpuFence>(*m_api, m_instance, m_device, initialValue);
            return ErrorCode::Ok;
        }
        Status CreateQuerySet(const QuerySetDesc& setDesc, QuerySet*& out) override
        {
            return CreateResource<WebGpuQuerySet>(
                out, [&](WebGpuQuerySet& q) { return q.Initialize(*m_api, m_device, setDesc); });
        }
        Status CreateSwapChain(Surface* surface, const SwapChainDesc& swapDesc,
                               SwapChain*& out) override
        {
            if (surface == nullptr)
            {
                out = nullptr;
                return ErrorCode::InvalidArgument;
            }
            return CreateResource<WebGpuSwapChain>(
                out, [&](WebGpuSwapChain& sc)
                { return sc.Initialize(*m_api, m_adapter, m_device, &m_graphicsQueue,
                                       static_cast<WebGpuSurface*>(surface), swapDesc); });
        }

        // ---- Resource destruction ----
        // Every path tolerates the nullptr a failed Create* handed out.
        void DestroyBuffer(Buffer*& x) override
        {
            if (x != nullptr)
            {
                m_bufferRegistry.Remove(static_cast<WebGpuBuffer*>(x));
            }
            ReleaseAndDelete<WebGpuBuffer>(x);
        }
        void DestroyTexture(Texture*& x) override { ReleaseAndDelete<WebGpuTexture>(x); }
        void DestroyTextureView(TextureView*& x) override
        {
            ReleaseAndDelete<WebGpuTextureView>(x);
        }
        void DestroySampler(Sampler*& x) override { ReleaseAndDelete<WebGpuSampler>(x); }
        void DestroyShaderModule(ShaderModule*& x) override
        {
            ReleaseAndDelete<WebGpuShaderModule>(x);
        }
        void DestroyBindGroupLayout(BindGroupLayout*& x) override
        {
            ReleaseAndDelete<WebGpuBindGroupLayout>(x);
        }
        void DestroyBindGroup(BindGroup*& x) override { ReleaseAndDelete<WebGpuBindGroup>(x); }
        void DestroyPipelineLayout(PipelineLayout*& x) override
        {
            ReleaseAndDelete<WebGpuPipelineLayout>(x);
        }
        void DestroyPipelineCache(PipelineCache*& x) override { DeleteIfAny(x); }
        void DestroyRenderPipeline(RenderPipeline*& x) override
        {
            ReleaseAndDelete<WebGpuRenderPipeline>(x);
        }
        void DestroyComputePipeline(ComputePipeline*& x) override
        {
            ReleaseAndDelete<WebGpuComputePipeline>(x);
        }
        void DestroyCommandPool(CommandPool*& x) override
        {
            if (x != nullptr)
            {
                auto* pool = static_cast<WebGpuCommandPool*>(x);
                m_allocator.Delete(pool);
                x = nullptr;
            }
        }
        void DestroyFence(Fence*& x) override { DeleteIfAny(x); }
        void DestroyQuerySet(QuerySet*& x) override { ReleaseAndDelete<WebGpuQuerySet>(x); }
        void DestroySwapChain(SwapChain*& x) override
        {
            if (x != nullptr)
            {
                auto* swapChain = static_cast<WebGpuSwapChain*>(x);
                swapChain->Cleanup();
                m_allocator.Delete(swapChain);
                x = nullptr;
            }
        }
        void DestroySurface(Surface*& x) override { ReleaseAndDelete<WebGpuSurface>(x); }

        // ---- Lifecycle ----
        bool IsLost() override { return m_lost; }

        void WaitIdle() override
        {
            // wgpu-native's DevicePoll(wait) drains the queue; on web (no poll
            // extension) the queue wrapper's callback pump does the same job.
            if (m_api->wgpuDevicePoll != nullptr)
            {
                (void)m_api->wgpuDevicePoll(m_device, 1u, nullptr);
            }
            else
            {
                m_graphicsQueue.WaitIdle();
            }
        }

        void Destroy() override
        {
            m_blitHelper.Release();
            m_api->wgpuQueueRelease(m_graphicsQueue.Handle());
            m_api->wgpuDeviceRelease(m_device);
            IAllocator& allocator = m_allocator;
            this->~WebGpuDevice();
            allocator.Free(this);
        }

    private:
        /// Allocate, run the init closure, roll back on failure - the one Create shape.
        template <typename TResource, typename TBase, typename TInit>
        Status CreateResource(TBase*& out, TInit&& initialize)
        {
            out = nullptr;
            auto* resource = m_allocator.New<TResource>();
            const Status status = initialize(*resource);
            if (!status.IsOk())
            {
                m_allocator.Delete(resource);
                return status;
            }
            out = resource;
            return ErrorCode::Ok;
        }

        template <typename TResource, typename TBase> void ReleaseAndDelete(TBase*& object)
        {
            if (object != nullptr)
            {
                auto* resource = static_cast<TResource*>(object);
                resource->Release();
                m_allocator.Delete(resource);
                object = nullptr;
            }
        }

        template <typename T> void DeleteIfAny(T*& object)
        {
            if (object != nullptr)
            {
                m_allocator.Delete(object);
                object = nullptr;
            }
        }

        const WebGpuApi* m_api;
        WGPUInstance m_instance;
        WGPUAdapter m_adapter;
        WGPUDevice m_device;
        IAllocator& m_allocator;
        WebGpuBlitHelper m_blitHelper;
        WebGpuBufferRegistry m_bufferRegistry;
        WebGpuQueue m_graphicsQueue;
        WebGpuQueue m_computeQueue;
        WebGpuQueue m_transferQueue;
        bool m_lost = false;
        bool m_immediatesSupported = false;
        bool m_forceUniformPushConstants = false;
        bool m_forceWgsl = false; // DRACONIC_WEBGPU_WGSL: use the browser's WGSL path on desktop
    };
}
