/// Null RHI backend - stub implementations for all interfaces.
/// Useful for headless testing, CI, or when no GPU is available.

module;
#include "Draconic.Foundation/Prelude.h" // <new> reachability for placement-new in core templates (GCC)

export module draconic.rhi.null;

import draconic.foundation;
import draconic.rhi;

using namespace draconic::foundation;

export namespace draconic::rhi::null
{

    // ---- Stub resource classes ----

    class NullBuffer : public Buffer
    {
    public:
        void* Map() override { return m_mapped; }
        void Unmap() override {}
        void allocate(u64 size)
        {
            m_data.Resize(static_cast<usize>(size));
            m_mapped = m_data.Data();
        }

    private:
        Array<u8> m_data;
        void* m_mapped = nullptr;
    };

    class NullTexture : public Texture
    {
    };
    class NullTextureView : public TextureView
    {
    };
    class NullSampler : public Sampler
    {
    };
    class NullShaderModule : public ShaderModule
    {
    };
    class NullSurface : public Surface
    {
    };
    class NullCommandBuffer : public CommandBuffer
    {
    };

    class NullFence : public Fence
    {
    public:
        u64 CompletedValue() override { return m_value; }
        bool Wait(u64 value, u64) override
        {
            m_value = value;
            return true;
        }
        void signal(u64 v) { m_value = v; }

    private:
        u64 m_value = 0;
    };

    class NullQuerySet : public QuerySet
    {
    };

    class NullBindGroupLayout : public BindGroupLayout
    {
    public:
        Span<const BindGroupLayoutEntry> Entries() const override { return {}; }
    };

    class NullBindGroup : public BindGroup
    {
    public:
        BindGroupLayout* Layout() override { return nullptr; }
        void UpdateBindless(Span<const BindlessUpdateEntry>) override {}
    };

    class NullPipelineLayout : public PipelineLayout
    {
    };

    class NullPipelineCache : public PipelineCache
    {
    public:
        u32 GetDataSize() override { return 0; }
        Status GetData(Span<u8>) override { return ErrorCode::Ok; }
    };

    class NullRenderPipeline : public RenderPipeline
    {
    };
    class NullComputePipeline : public ComputePipeline
    {
    };
    class NullMeshPipeline : public MeshPipeline
    {
    };

    class NullAccelStruct : public AccelStruct
    {
    public:
        AccelStructType Type() const override { return AccelStructType::BottomLevel; }
        u64 DeviceAddress() const override { return 0; }
    };

    class NullRayTracingPipeline : public RayTracingPipeline
    {
    };

    // ---- Stub encoders ----

    class NullRenderPassEncoder : public RenderPassEncoder, public MeshShaderPassExt
    {
    public:
        MeshShaderPassExt* AsMeshShaderExt() noexcept override { return this; }
        void SetPipeline(RenderPipeline*) override {}
        void SetBindGroup(u32, BindGroup*, Span<const u32>) override {}
        void SetPushConstants(ShaderStage, u32, u32, const void*) override {}
        void SetVertexBuffer(u32, Buffer*, u64) override {}
        void SetIndexBuffer(Buffer*, IndexFormat, u64) override {}
        void SetViewport(f32, f32, f32, f32, f32, f32) override {}
        void SetScissor(i32, i32, u32, u32) override {}
        void SetBlendConstant(f32, f32, f32, f32) override {}
        void SetStencilReference(u32) override {}
        void Draw(u32, u32, u32, u32) override {}
        void DrawIndexed(u32, u32, u32, i32, u32) override {}
        void DrawIndirect(Buffer*, u64, u32, u32) override {}
        void DrawIndexedIndirect(Buffer*, u64, u32, u32) override {}
        void ExecuteBundles(Span<RenderBundle* const>) override {}
        void WriteTimestamp(QuerySet*, u32) override {}
        void BeginOcclusionQuery(QuerySet*, u32) override {}
        void EndOcclusionQuery(QuerySet*, u32) override {}
        void End() override {}
        void SetMeshPipeline(MeshPipeline*) override {}
        void DrawMeshTasks(u32, u32, u32) override {}
        void DrawMeshTasksIndirect(Buffer*, u64, u32, u32) override {}
        void DrawMeshTasksIndirectCount(Buffer*, u64, Buffer*, u64, u32, u32) override {}
    };

    class NullRenderBundle : public RenderBundle
    {
    };

    class NullRenderBundleEncoder : public RenderBundleEncoder
    {
    public:
        void SetPipeline(RenderPipeline*) override {}
        void SetBindGroup(u32, BindGroup*, Span<const u32>) override {}
        void SetPushConstants(ShaderStage, u32, u32, const void*) override {}
        void SetVertexBuffer(u32, Buffer*, u64) override {}
        void SetIndexBuffer(Buffer*, IndexFormat, u64) override {}
        void Draw(u32, u32, u32, u32) override {}
        void DrawIndexed(u32, u32, u32, i32, u32) override {}
        void DrawIndirect(Buffer*, u64, u32, u32) override {}
        void DrawIndexedIndirect(Buffer*, u64, u32, u32) override {}
        RenderBundle* Finish() override { return &bundle; }
        NullRenderBundle bundle;
    };

    class NullComputePassEncoder : public ComputePassEncoder
    {
    public:
        void SetPipeline(ComputePipeline*) override {}
        void SetBindGroup(u32, BindGroup*, Span<const u32>) override {}
        void SetPushConstants(ShaderStage, u32, u32, const void*) override {}
        void Dispatch(u32, u32, u32) override {}
        void DispatchIndirect(Buffer*, u64) override {}
        void ComputeBarrier() override {}
        void WriteTimestamp(QuerySet*, u32) override {}
        void End() override {}
    };

    class NullCommandEncoder : public CommandEncoder, public RayTracingEncoderExt
    {
    public:
        RayTracingEncoderExt* AsRayTracingExt() noexcept override { return this; }
        NullRenderPassEncoder rpe;
        NullComputePassEncoder cpe;
        NullRenderBundleEncoder rbe;
        NullCommandBuffer cb;

        RenderPassEncoder* BeginRenderPass(const RenderPassDesc&) override { return &rpe; }
        ComputePassEncoder* BeginComputePass(StringView) override { return &cpe; }
        RenderBundleEncoder* CreateRenderBundleEncoder(const RenderBundleDesc&) override
        {
            return &rbe;
        }
        void Barrier(const BarrierGroup&) override {}
        void CopyBufferToBuffer(Buffer*, u64, Buffer*, u64, u64) override {}
        void CopyBufferToTexture(Buffer*, Texture*, const BufferTextureCopyRegion&) override {}
        void CopyTextureToBuffer(Texture*, Buffer*, const BufferTextureCopyRegion&) override {}
        void CopyTextureToTexture(Texture*, Texture*, const TextureCopyRegion&) override {}
        void Blit(Texture*, Texture*) override {}
        void GenerateMipmaps(Texture*) override {}
        void ResolveTexture(Texture*, Texture*) override {}
        void ResetQuerySet(QuerySet*, u32, u32) override {}
        void WriteTimestamp(QuerySet*, u32) override {}
        void ResolveQuerySet(QuerySet*, u32, u32, Buffer*, u64) override {}
        void BeginDebugLabel(StringView, f32, f32, f32, f32) override {}
        void EndDebugLabel() override {}
        void InsertDebugLabel(StringView, f32, f32, f32, f32) override {}
        CommandBuffer* Finish() override { return &cb; }

        // RayTracingEncoderExt
        void BuildBottomLevelAccelStruct(AccelStruct*, Buffer*, u64,
                                         Span<const AccelStructGeometryTriangles>,
                                         Span<const AccelStructGeometryAABBs>) override
        {
        }
        void BuildTopLevelAccelStruct(AccelStruct*, Buffer*, u64, Buffer*, u64, u32) override {}
        void SetRayTracingPipeline(RayTracingPipeline*) override {}
        void SetBindGroup(u32, BindGroup*, Span<const u32>) override {}
        void SetPushConstants(ShaderStage, u32, u32, const void*) override {}
        void TraceRays(Buffer*, u64, u64, Buffer*, u64, u64, Buffer*, u64, u64, u32, u32,
                       u32) override
        {
        }
    };

    class NullCommandPool : public CommandPool
    {
    public:
        NullCommandEncoder enc;
        Status CreateEncoder(CommandEncoder*& out) override
        {
            out = &enc;
            return ErrorCode::Ok;
        }
        void DestroyEncoder(CommandEncoder*&) override {}
        void Reset() override {}
        RenderBundleEncoder* CreateRenderBundleEncoder(const RenderBundleDesc&) override
        {
            return &enc.rbe;
        }
    };

    class NullTransferBatch : public TransferBatch
    {
    public:
        void WriteBuffer(Buffer*, u64, Span<const u8>) override {}
        void WriteTexture(Texture*, Span<const u8>, const TextureDataLayout&, Extent3D, u32,
                          u32) override
        {
        }
        Status Submit() override { return ErrorCode::Ok; }
        Status SubmitAsync(Fence*, u64) override { return ErrorCode::Ok; }
        void Reset() override {}
        void Destroy() override {}
    };

    class NullSwapChain : public SwapChain
    {
    public:
        NullTexture tex;
        NullTextureView view;
        TextureFormat m_format = TextureFormat::BGRA8UnormSrgb;
        u32 m_width = 0;
        u32 m_height = 0;
        u32 m_count = 2;
        u32 m_imgIdx = 0;

        TextureFormat Format() const override { return m_format; }
        u32 Width() const override { return m_width; }
        u32 Height() const override { return m_height; }
        u32 BufferCount() const override { return m_count; }
        u32 CurrentImageIndex() const override { return m_imgIdx; }
        Status AcquireNextImage() override
        {
            m_imgIdx = (m_imgIdx + 1) % m_count;
            return ErrorCode::Ok;
        }
        Texture* CurrentTexture() override { return &tex; }
        TextureView* CurrentTextureView() override { return &view; }
        Status Present(Queue*) override { return ErrorCode::Ok; }
        Status Resize(u32 w, u32 h) override
        {
            m_width = w;
            m_height = h;
            return ErrorCode::Ok;
        }
    };

    class NullQueue : public Queue
    {
    public:
        NullTransferBatch tb;
        void Submit(Span<CommandBuffer* const>) override {}
        void Submit(Span<CommandBuffer* const>, Fence* f, u64 v) override
        {
            if (auto* nf = static_cast<NullFence*>(f))
                nf->signal(v);
        }
        void Submit(Span<CommandBuffer* const>, Span<Fence* const>, Span<const u64>, Fence* f,
                    u64 v) override
        {
            if (auto* nf = static_cast<NullFence*>(f))
                nf->signal(v);
        }
        void WaitIdle() override {}
        Status CreateTransferBatch(TransferBatch*& out) override
        {
            out = &tb;
            return ErrorCode::Ok;
        }
        void DestroyTransferBatch(TransferBatch*&) override {}
        f32 TimestampPeriod() const override { return 1.0f; }
    };

    // ---- Null Device ----

    class NullDevice : public Device
    {
    public:
        NullQueue gfxQueue, compQueue, xferQueue;
        IAllocator& m_allocator;

        explicit NullDevice(IAllocator& allocator) : m_allocator(allocator)
        {
            type = DeviceType::Null;
            gfxQueue.queueType = QueueType::Graphics;
            compQueue.queueType = QueueType::Compute;
            xferQueue.queueType = QueueType::Transfer;
        }

        [[nodiscard]] ShaderFormat PreferredShaderFormat() const noexcept override
        {
            return ShaderFormat::SpirV;
        }

        [[nodiscard]] bool NeedsClipSpaceYFlip() const noexcept override { return false; }

        Queue* GetQueue(QueueType t, u32) override
        {
            switch (t)
            {
            case QueueType::Graphics:
                return &gfxQueue;
            case QueueType::Compute:
                return &compQueue;
            case QueueType::Transfer:
                return &xferQueue;
            }
            return nullptr;
        }
        u32 GetQueueCount(QueueType) override { return 1; }
        FormatSupport GetFormatSupport(TextureFormat) override
        {
            return FormatSupport::Texture | FormatSupport::ColorAttachment |
                   FormatSupport::DepthStencil;
        }

        Status CreateBuffer(const BufferDesc& d, Buffer*& out) override
        {
            auto* b = m_allocator.New<NullBuffer>();
            b->desc = d;
            b->allocate(d.size);
            out = b;
            return ErrorCode::Ok;
        }
        Status CreateTexture(const TextureDesc& d, Texture*& out) override
        {
            auto* t = m_allocator.New<NullTexture>();
            t->desc = d;
            out = t;
            return ErrorCode::Ok;
        }
        Status CreateTextureView(Texture* tex, const TextureViewDesc& d, TextureView*& out) override
        {
            auto* v = m_allocator.New<NullTextureView>();
            v->desc = d;
            v->texture = tex;
            out = v;
            return ErrorCode::Ok;
        }
        Status CreateSampler(const SamplerDesc& d, Sampler*& out) override
        {
            auto* s = m_allocator.New<NullSampler>();
            s->desc = d;
            out = s;
            return ErrorCode::Ok;
        }
        Status CreateShaderModule(const ShaderModuleDesc&, ShaderModule*& out) override
        {
            out = m_allocator.New<NullShaderModule>();
            return ErrorCode::Ok;
        }
        Status CreateBindGroupLayout(const BindGroupLayoutDesc&, BindGroupLayout*& out) override
        {
            out = m_allocator.New<NullBindGroupLayout>();
            return ErrorCode::Ok;
        }
        Status CreateBindGroup(const BindGroupDesc&, BindGroup*& out) override
        {
            out = m_allocator.New<NullBindGroup>();
            return ErrorCode::Ok;
        }
        Status CreatePipelineLayout(const PipelineLayoutDesc&, PipelineLayout*& out) override
        {
            out = m_allocator.New<NullPipelineLayout>();
            return ErrorCode::Ok;
        }
        Status CreatePipelineCache(const PipelineCacheDesc&, PipelineCache*& out) override
        {
            out = m_allocator.New<NullPipelineCache>();
            return ErrorCode::Ok;
        }
        Status CreateRenderPipeline(const RenderPipelineDesc&, RenderPipeline*& out) override
        {
            out = m_allocator.New<NullRenderPipeline>();
            return ErrorCode::Ok;
        }
        Status CreateComputePipeline(const ComputePipelineDesc&, ComputePipeline*& out) override
        {
            out = m_allocator.New<NullComputePipeline>();
            return ErrorCode::Ok;
        }
        Status CreateCommandPool(QueueType, CommandPool*& out) override
        {
            out = m_allocator.New<NullCommandPool>();
            return ErrorCode::Ok;
        }
        Status CreateFence(u64, Fence*& out) override
        {
            out = m_allocator.New<NullFence>();
            return ErrorCode::Ok;
        }
        Status CreateQuerySet(const QuerySetDesc& d, QuerySet*& out) override
        {
            auto* q = m_allocator.New<NullQuerySet>();
            q->type = d.type;
            q->count = d.count;
            out = q;
            return ErrorCode::Ok;
        }
        Status CreateSwapChain(Surface*, const SwapChainDesc& d, SwapChain*& out) override
        {
            auto* sc = m_allocator.New<NullSwapChain>();
            sc->m_format = d.format;
            sc->m_width = d.width;
            sc->m_height = d.height;
            sc->m_count = d.bufferCount;
            out = sc;
            return ErrorCode::Ok;
        }

        void DestroyBuffer(Buffer*& x) override
        {
            m_allocator.Delete(x);
            x = nullptr;
        }
        void DestroyTexture(Texture*& x) override
        {
            m_allocator.Delete(x);
            x = nullptr;
        }
        void DestroyTextureView(TextureView*& x) override
        {
            m_allocator.Delete(x);
            x = nullptr;
        }
        void DestroySampler(Sampler*& x) override
        {
            m_allocator.Delete(x);
            x = nullptr;
        }
        void DestroyShaderModule(ShaderModule*& x) override
        {
            m_allocator.Delete(x);
            x = nullptr;
        }
        void DestroyBindGroupLayout(BindGroupLayout*& x) override
        {
            m_allocator.Delete(x);
            x = nullptr;
        }
        void DestroyBindGroup(BindGroup*& x) override
        {
            m_allocator.Delete(x);
            x = nullptr;
        }
        void DestroyPipelineLayout(PipelineLayout*& x) override
        {
            m_allocator.Delete(x);
            x = nullptr;
        }
        void DestroyPipelineCache(PipelineCache*& x) override
        {
            m_allocator.Delete(x);
            x = nullptr;
        }
        void DestroyRenderPipeline(RenderPipeline*& x) override
        {
            m_allocator.Delete(x);
            x = nullptr;
        }
        void DestroyComputePipeline(ComputePipeline*& x) override
        {
            m_allocator.Delete(x);
            x = nullptr;
        }
        void DestroyCommandPool(CommandPool*& x) override
        {
            m_allocator.Delete(x);
            x = nullptr;
        }
        void DestroyFence(Fence*& x) override
        {
            m_allocator.Delete(x);
            x = nullptr;
        }
        void DestroyQuerySet(QuerySet*& x) override
        {
            m_allocator.Delete(x);
            x = nullptr;
        }
        void DestroySwapChain(SwapChain*& x) override
        {
            m_allocator.Delete(x);
            x = nullptr;
        }
        void DestroySurface(Surface*& x) override
        {
            m_allocator.Delete(x);
            x = nullptr;
        }

        bool IsLost() override { return false; } // a null device cannot be lost
        void WaitIdle() override {}
        void Destroy() override
        {
            IAllocator& alloc = m_allocator;
            this->~NullDevice();
            alloc.Free(this);
        }
    };

    // ---- Null Adapter ----

    class NullAdapter : public Adapter
    {
    public:
        IAllocator& m_allocator;

        explicit NullAdapter(IAllocator& allocator) : m_allocator(allocator) {}

        void GetInfo(AdapterInfo& out) override
        {
            out.name = u8"Null Device";
            out.vendorId = 0;
            out.deviceId = 0;
            out.type = AdapterType::Cpu;
        }
        Status CreateDevice(const DeviceDesc&, Device*& out) override
        {
            out = m_allocator.New<NullDevice>(m_allocator);
            return ErrorCode::Ok;
        }
    };

    // ---- Null Backend ----

    class NullBackend : public Backend
    {
    public:
        IAllocator& m_allocator;
        NullAdapter adapter;
        Adapter* adapterPtr = &adapter;

        explicit NullBackend(IAllocator& allocator) : m_allocator(allocator), adapter(allocator) {}

        Span<Adapter* const> EnumerateAdapters() override
        {
            return Span<Adapter* const>(&adapterPtr, 1);
        }

        Status CreateSurface(void*, void*, Surface*& out,
                             SurfacePlatform = SurfacePlatform::Unknown) override
        {
            out = m_allocator.New<NullSurface>();
            return ErrorCode::Ok;
        }

        void Destroy() override
        {
            IAllocator& alloc = m_allocator;
            this->~NullBackend();
            alloc.Free(this);
        }
    };

    /// Creates a null backend for headless / GPU-less testing.
    Status CreateNullBackend(Backend*& out, IAllocator& allocator = DefaultAllocator())
    {
        auto* b = allocator.New<NullBackend>(allocator);
        b->isInitialized = true;
        out = b;
        return ErrorCode::Ok;
    }

} // namespace draconic::rhi::null
