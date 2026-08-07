/// Descriptor structs for creating RHI resources, pipelines, and render passes.

module;

export module draconic.rhi:descriptors;

import draconic.foundation;
import :enums;
import :texture_format;
import :types;
import :forward;

using namespace draconic::foundation;

export namespace draconic::rhi
{

    // ---- Resources ----

    struct BufferDesc
    {
        u64 size = 0;
        BufferUsage usage = BufferUsage::None;
        MemoryLocation memory = MemoryLocation::GpuOnly;
        StringView label;
    };

    struct TextureDesc
    {
        TextureDimension dimension = TextureDimension::Texture2D;
        TextureFormat format = TextureFormat::Undefined;
        u32 width = 1, height = 1, depth = 1;
        u32 arrayLayerCount = 1;
        u32 mipLevelCount = 1;
        u32 sampleCount = 1;
        TextureUsage usage = TextureUsage::None;
        StringView label;

        /// Convenience factory for a 2D render target.
        static TextureDesc RenderTarget(TextureFormat fmt, u32 w, u32 h, u32 samples = 1,
                                        StringView lbl = {})
        {
            TextureDesc d{};
            d.format = fmt;
            d.width = w;
            d.height = h;
            d.sampleCount = samples;
            d.usage = TextureUsage::RenderTarget | TextureUsage::Sampled;
            d.label = lbl;
            return d;
        }

        /// Convenience factory for a depth buffer.
        static TextureDesc DepthBuffer(TextureFormat fmt, u32 w, u32 h, u32 samples = 1,
                                       StringView lbl = {})
        {
            TextureDesc d{};
            d.format = fmt;
            d.width = w;
            d.height = h;
            d.sampleCount = samples;
            d.usage = TextureUsage::DepthStencil;
            d.label = lbl;
            return d;
        }
    };

    struct TextureViewDesc
    {
        TextureFormat format = TextureFormat::Undefined;
        TextureViewDimension dimension = TextureViewDimension::Texture2D;
        u32 baseMipLevel = 0;
        u32 mipLevelCount = 1;
        u32 baseArrayLayer = 0;
        u32 arrayLayerCount = 1;
        TextureAspect aspect = TextureAspect::All;
        StringView label;
    };

    struct SamplerDesc
    {
        FilterMode minFilter = FilterMode::Linear;
        FilterMode magFilter = FilterMode::Linear;
        MipmapFilterMode mipmapFilter = MipmapFilterMode::Linear;
        AddressMode addressU = AddressMode::Repeat;
        AddressMode addressV = AddressMode::Repeat;
        AddressMode addressW = AddressMode::Repeat;
        f32 mipLodBias = 0.0f;
        f32 minLod = 0.0f;
        f32 maxLod = 1000.0f;
        u16 maxAnisotropy = 1;
        Optional<CompareFunction> compare;
        SamplerBorderColor borderColor = SamplerBorderColor::TransparentBlack;
        StringView label;
    };

    struct ShaderModuleDesc
    {
        Span<const u8> code; ///< SPIR-V or DXIL bytecode.
        StringView label;
    };

    struct QuerySetDesc
    {
        QueryType type = QueryType::Timestamp;
        u32 count = 0;
        StringView label;
    };

    struct SwapChainDesc
    {
        u32 width = 0;
        u32 height = 0;
        TextureFormat format = TextureFormat::BGRA8UnormSrgb;
        PresentMode presentMode = PresentMode::Fifo;
        u32 bufferCount = 2;
        StringView label;
    };

    // ---- Binding ----

    /// Describes one entry in a bind group layout.
    struct BindGroupLayoutEntry
    {
        u32 binding = 0;
        ShaderStage visibility = ShaderStage::None;
        BindingType type = BindingType::UniformBuffer;
        TextureViewDimension textureDimension = TextureViewDimension::Texture2D;
        TextureSampleType textureSampleType = TextureSampleType::Float;
        // WebGPU-only: a Sampler entry that binds nearest-only samplers (required
        // when the paired texture is UnfilterableFloat, e.g. depth read as data).
        // Vulkan/DX12 have no such distinction and ignore it.
        bool samplerNonFiltering = false;
        bool textureMultisampled = false;
        TextureFormat storageTextureFormat = TextureFormat::Undefined;
        bool hasDynamicOffset = false;
        u32 storageBufferStride = 0;
        u32 count = 1;
        StringView label;

        /// Factory: uniform buffer binding.
        static BindGroupLayoutEntry UniformBuffer(u32 binding, ShaderStage vis)
        {
            BindGroupLayoutEntry e{};
            e.binding = binding;
            e.visibility = vis;
            e.type = BindingType::UniformBuffer;
            return e;
        }

        /// Factory: sampled texture binding.
        static BindGroupLayoutEntry
        SampledTexture(u32 binding, ShaderStage vis,
                       TextureViewDimension dim = TextureViewDimension::Texture2D)
        {
            BindGroupLayoutEntry e{};
            e.binding = binding;
            e.visibility = vis;
            e.type = BindingType::SampledTexture;
            e.textureDimension = dim;
            return e;
        }

        /// Factory: sampler binding.
        static BindGroupLayoutEntry Sampler(u32 binding, ShaderStage vis)
        {
            BindGroupLayoutEntry e{};
            e.binding = binding;
            e.visibility = vis;
            e.type = BindingType::Sampler;
            return e;
        }

        /// Factory: storage buffer binding. `stride` is the HLSL element size when the
        /// shader declares (RW)StructuredBuffer<T> — REQUIRED on DX12, where the SRV/UAV
        /// descriptor is built structured (stride) or raw (0, ByteAddressBuffer). A raw
        /// descriptor bound to a StructuredBuffer<T> shader is undefined behavior on DX12
        /// (garbage reads). Vulkan ignores it (SSBOs are unstructured).
        static BindGroupLayoutEntry StorageBuffer(u32 binding, ShaderStage vis,
                                                  bool readOnly = false, u32 stride = 0)
        {
            BindGroupLayoutEntry e{};
            e.binding = binding;
            e.visibility = vis;
            e.type =
                readOnly ? BindingType::StorageBufferReadOnly : BindingType::StorageBufferReadWrite;
            e.storageBufferStride = stride;
            return e;
        }
    };

    struct BindGroupLayoutDesc
    {
        Span<const BindGroupLayoutEntry> entries;
        StringView label;
    };

    /// Describes one resource binding within a bind group.
    struct BindGroupEntry
    {
        Buffer* buffer = nullptr;
        u64 bufferOffset = 0;
        u64 bufferSize = 0;
        TextureView* textureView = nullptr;
        Sampler* sampler = nullptr;
        AccelStruct* accelStruct = nullptr;

        static BindGroupEntry BufferEntry(Buffer* buf, u64 offset, u64 size)
        {
            BindGroupEntry e{};
            e.buffer = buf;
            e.bufferOffset = offset;
            e.bufferSize = size;
            return e;
        }
        static BindGroupEntry TextureEntry(TextureView* view)
        {
            BindGroupEntry e{};
            e.textureView = view;
            return e;
        }
        static BindGroupEntry SamplerEntry(Sampler* s)
        {
            BindGroupEntry e{};
            e.sampler = s;
            return e;
        }
        static BindGroupEntry AccelStructEntry(AccelStruct* as)
        {
            BindGroupEntry e{};
            e.accelStruct = as;
            return e;
        }
    };

    struct BindGroupDesc
    {
        BindGroupLayout* layout = nullptr;
        Span<const BindGroupEntry> entries;
        StringView label;
    };

    /// For updating individual entries in a bindless bind group.
    struct BindlessUpdateEntry
    {
        u32 layoutIndex = 0;
        u32 arrayIndex = 0;
        Buffer* buffer = nullptr;
        u64 bufferOffset = 0;
        u64 bufferSize = 0;
        TextureView* textureView = nullptr;
        Sampler* sampler = nullptr;
    };

    // ---- Pipelines ----

    struct PipelineLayoutDesc
    {
        Span<BindGroupLayout* const> bindGroupLayouts;
        Span<const PushConstantRange> pushConstantRanges;
        StringView label;
    };

    struct PipelineCacheDesc
    {
        Span<const u8> initialData;
        StringView label;
    };

    struct VertexAttribute
    {
        VertexFormat format = VertexFormat::Float32;
        u32 offset = 0;
        u32 shaderLocation = 0;
    };

    struct VertexBufferLayout
    {
        u32 stride = 0;
        VertexStepMode stepMode = VertexStepMode::Vertex;
        Span<const VertexAttribute> attributes;
    };

    struct BlendComponent
    {
        BlendFactor srcFactor = BlendFactor::One;
        BlendFactor dstFactor = BlendFactor::Zero;
        BlendOperation operation = BlendOperation::Add;
    };

    struct BlendState
    {
        BlendComponent color;
        BlendComponent alpha;

        // Standard alpha blending: srcAlpha*src + (1-srcAlpha)*dst.
        static constexpr BlendState AlphaBlend()
        {
            return {{BlendFactor::SrcAlpha, BlendFactor::OneMinusSrcAlpha, BlendOperation::Add},
                    {BlendFactor::One, BlendFactor::OneMinusSrcAlpha, BlendOperation::Add}};
        }
        // Premultiplied alpha: src + (1-srcAlpha)*dst.
        static constexpr BlendState PremultipliedAlpha()
        {
            return {{BlendFactor::One, BlendFactor::OneMinusSrcAlpha, BlendOperation::Add},
                    {BlendFactor::One, BlendFactor::OneMinusSrcAlpha, BlendOperation::Add}};
        }
        // Additive: src + dst - accumulate, e.g. the bloom upsample chain.
        static constexpr BlendState Additive()
        {
            return {{BlendFactor::One, BlendFactor::One, BlendOperation::Add},
                    {BlendFactor::One, BlendFactor::One, BlendOperation::Add}};
        }
        // Multiply: src * dst.
        static constexpr BlendState Multiply()
        {
            return {{BlendFactor::Dst, BlendFactor::Zero, BlendOperation::Add},
                    {BlendFactor::DstAlpha, BlendFactor::Zero, BlendOperation::Add}};
        }
    };

    struct ColorTargetState
    {
        TextureFormat format = TextureFormat::Undefined;
        Optional<BlendState> blend;
        ColorWriteMask writeMask = ColorWriteMask::All;
    };

    struct StencilFaceState
    {
        CompareFunction compare = CompareFunction::Always;
        StencilOperation failOp = StencilOperation::Keep;
        StencilOperation depthFailOp = StencilOperation::Keep;
        StencilOperation passOp = StencilOperation::Keep;
    };

    struct DepthStencilState
    {
        TextureFormat format = TextureFormat::Undefined;
        bool depthTestEnabled = true;
        bool depthWriteEnabled = true;
        CompareFunction depthCompare = CompareFunction::Less;
        bool stencilEnabled = false;
        u8 stencilReadMask = 0xFF;
        u8 stencilWriteMask = 0xFF;
        StencilFaceState stencilFront;
        StencilFaceState stencilBack;
        i32 depthBias = 0;
        f32 depthBiasSlopeScale = 0.0f;
        f32 depthBiasClamp = 0.0f;
    };

    struct PrimitiveState
    {
        PrimitiveTopology topology = PrimitiveTopology::TriangleList;
        FrontFace frontFace = FrontFace::CCW;
        CullMode cullMode = CullMode::None;
        FillMode fillMode = FillMode::Solid;
        bool depthClipEnabled = true;
    };

    struct MultisampleState
    {
        u32 count = 1;
        u32 mask = 0xFFFFFFFF;
        bool alphaToCoverageEnabled = false;
    };

    /// Programmable shader stage (vertex, fragment, compute, mesh, task, etc.).
    struct ProgrammableStage
    {
        ShaderModule* module = nullptr;
        StringView entryPoint = u8"main";
        ShaderStage stage = ShaderStage::None;
    };

    struct VertexState
    {
        ProgrammableStage shader;
        Span<const VertexBufferLayout> buffers;
    };

    struct FragmentState
    {
        ProgrammableStage shader;
        Span<const ColorTargetState> targets;
    };

    /// Descriptor for creating a graphics (render) pipeline.
    struct RenderPipelineDesc
    {
        PipelineLayout* layout = nullptr;
        VertexState vertex;
        Optional<FragmentState> fragment;
        PrimitiveState primitive;
        Optional<DepthStencilState> depthStencil;
        MultisampleState multisample;
        PipelineCache* cache = nullptr;
        StringView label;
    };

    /// Descriptor for creating a compute pipeline.
    struct ComputePipelineDesc
    {
        PipelineLayout* layout = nullptr;
        ProgrammableStage compute;
        PipelineCache* cache = nullptr;
        StringView label;
    };

    // ---- Render pass ----

    /// Color attachment for a render pass.
    struct ColorAttachment
    {
        TextureView* view = nullptr;
        TextureView* resolveTarget = nullptr;
        LoadOp loadOp = LoadOp::Clear;
        StoreOp storeOp = StoreOp::Store;
        ClearColor clearValue = ClearColor::Black();
    };

    /// Depth/stencil attachment for a render pass.
    struct DepthStencilAttachment
    {
        TextureView* view = nullptr;
        LoadOp depthLoadOp = LoadOp::Clear;
        StoreOp depthStoreOp = StoreOp::Store;
        f32 depthClearValue = 1.0f;
        bool depthReadOnly = false;
        LoadOp stencilLoadOp = LoadOp::Clear;
        StoreOp stencilStoreOp = StoreOp::Store;
        u32 stencilClearValue = 0;
        bool stencilReadOnly = false;
    };

    /// Color attachment list (stack-allocated, up to MaxColorAttachments).
    using ColorAttachmentList = FixedArray<ColorAttachment, MaxColorAttachments>;

    /// Descriptor for beginning a render pass.
    struct RenderPassDesc
    {
        ColorAttachmentList colorAttachments;
        Optional<DepthStencilAttachment> depthStencilAttachment;
        QuerySet* timestampQuerySet = nullptr;
        u32 beginTimestampIndex = 0;
        u32 endTimestampIndex = 0;
        // The query set Begin/EndOcclusionQuery will target. WebGPU REQUIRES the
        // declaration at pass begin; Vulkan/DX12 receive the set in the Begin call
        // itself and need nothing here (the validation layer may cross-check).
        QuerySet* occlusionQuerySet = nullptr;
        // How draws are supplied. Default Inline; set SecondaryCommandBuffers to execute render
        // bundles into this pass (Vulkan needs to know at begin time; other backends ignore it).
        RenderPassContents contents = RenderPassContents::Inline;
        StringView label;
    };

    // Describes a render bundle's target signature so it can be validated against - and replayed
    // into - compatible render passes: the attachment formats + sample count it records for. The
    // render-area extent lets the Vulkan backend record a full-target viewport/scissor into the
    // secondary command buffer (bundles carry no pass-level dynamic state; other backends inherit
    // it from the pass and ignore the extent).
    struct RenderBundleDesc
    {
        TextureFormat colorFormats[MaxColorAttachments] = {TextureFormat::Undefined};
        u32 colorFormatCount = 0;
        TextureFormat depthStencilFormat = TextureFormat::Undefined;
        // Whether the bundle leaves the aspect unwritten. WebGPU VALIDATES these
        // against the executing pass (a read-only pass only accepts read-only
        // bundles); Vulkan/DX12 bundles carry no such state and ignore them.
        bool depthReadOnly = false;
        bool stencilReadOnly = false;
        u32 sampleCount = 1;
        // The bundle's viewport/scissor (a sub-rect of the target for split-screen). x/y default to 0;
        // width/height are the viewport extent. A Vulkan secondary / DX12 bundle records this up front
        // since it can't inherit dynamic viewport state from the parent.
        i32 viewportX = 0;
        i32 viewportY = 0;
        u32 width = 0;
        u32 height = 0;
        StringView label;
    };

    // ---- Barriers ----

    struct BufferBarrier
    {
        Buffer* buffer = nullptr;
        ResourceState oldState = ResourceState::Undefined;
        ResourceState newState = ResourceState::Undefined;
        u64 offset = 0;
        u64 size = ~0ull;
    };

    struct TextureBarrier
    {
        Texture* texture = nullptr;
        ResourceState oldState = ResourceState::Undefined;
        ResourceState newState = ResourceState::Undefined;
        u32 baseMipLevel = 0;
        u32 mipLevelCount = ~0u;
        u32 baseArrayLayer = 0;
        u32 arrayLayerCount = ~0u;
    };

    struct MemoryBarrier
    {
        ResourceState oldState = ResourceState::Undefined;
        ResourceState newState = ResourceState::Undefined;
    };

    struct BarrierGroup
    {
        Span<const BufferBarrier> bufferBarriers;
        Span<const TextureBarrier> textureBarriers;
        Span<const MemoryBarrier> memoryBarriers;
    };

} // namespace draconic::rhi
