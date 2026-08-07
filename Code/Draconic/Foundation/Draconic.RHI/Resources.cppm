/// Abstract resource classes. Each represents a GPU-allocated object
/// owned and destroyed by the Device.

export module draconic.rhi:resources;

import draconic.foundation;
import :enums;
import :texture_format;
import :types;
import :descriptors;
import :ext_descriptors;

using namespace draconic::foundation;

export namespace draconic::rhi
{

    /// GPU buffer (vertex, index, uniform, storage, etc.).
    class Buffer
    {
    public:
        virtual ~Buffer() = default;

        BufferDesc desc{};

        [[nodiscard]] u64 GetSize() const { return desc.size; }
        [[nodiscard]] BufferUsage Usage() const { return desc.usage; }

        /// Map the buffer for CPU access. Returns nullptr if not mappable.
        [[nodiscard]] virtual void* Map() = 0;
        virtual void Unmap() = 0;
    };

    /// GPU texture (1D/2D/3D, with mip levels and array layers).
    class Texture
    {
    public:
        virtual ~Texture() = default;

        TextureDesc desc{};
        ResourceState initialState = ResourceState::Undefined;
    };

    /// Next monotonic TextureView::uniqueId (thread-safe; ids never repeat). Defined in
    /// ResourcesImpl.cpp - the <atomic> it needs must stay OUT of this interface unit
    /// (GCC gcm-cluster hygiene).
    [[nodiscard]] u64 NextTextureViewUniqueId() noexcept;

    /// A view into a subset of a texture's mip levels and array layers.
    class TextureView
    {
    public:
        virtual ~TextureView() = default;

        TextureViewDesc desc{};
        Texture* texture = nullptr;
        // Monotonic creation stamp. A destroyed view's ADDRESS can be reused by the very next
        // allocation, so any cache keyed by TextureView* MUST validate this id on a hit.
        // (Bind-group caches that trusted the raw pointer handed out descriptors of a
        // destroyed UI render target -> sampled a dead image view -> VK_ERROR_DEVICE_LOST.)
        u64 uniqueId = NextTextureViewUniqueId();
    };

    /// Texture sampler (filtering, addressing, comparison).
    class Sampler
    {
    public:
        virtual ~Sampler() = default;
        SamplerDesc desc{};
    };

    /// Compiled shader module (SPIR-V or DXIL bytecode).
    class ShaderModule
    {
    public:
        virtual ~ShaderModule() = default;
    };

    /// Presentation surface created from a native window handle.
    class Surface
    {
    public:
        virtual ~Surface() = default;
    };

    /// Immutable recorded command buffer, ready for queue submission.
    class CommandBuffer
    {
    public:
        virtual ~CommandBuffer() = default;
    };

    /// Timeline fence for CPU/GPU synchronization.
    class Fence
    {
    public:
        virtual ~Fence() = default;

        /// Returns the most recently completed (signaled) value.
        [[nodiscard]] virtual u64 CompletedValue() = 0;

        /// Blocks the CPU until the fence reaches `value`, or until timeout.
        /// Returns true on success, false on timeout.
        virtual bool Wait(u64 value, u64 timeoutNs = ~0ull) = 0;
    };

    /// GPU query set (timestamp, occlusion, pipeline statistics).
    class QuerySet
    {
    public:
        virtual ~QuerySet() = default;

        QueryType type = QueryType::Timestamp;
        u32 count = 0;
    };

    /// Defines the layout of resource bindings for a bind group.
    class BindGroupLayout
    {
    public:
        virtual ~BindGroupLayout() = default;

        [[nodiscard]] virtual Span<const BindGroupLayoutEntry> Entries() const = 0;
    };

    /// A set of resource bindings that can be bound to a pipeline.
    class BindGroup
    {
    public:
        virtual ~BindGroup() = default;

        [[nodiscard]] virtual BindGroupLayout* Layout() = 0;

        /// Update individual entries in a bindless bind group.
        virtual void UpdateBindless(Span<const BindlessUpdateEntry> entries) = 0;
    };

    /// Describes the resource binding layout for a pipeline (bind group
    /// layouts + push constant ranges).
    class PipelineLayout
    {
    public:
        virtual ~PipelineLayout() = default;
    };

    /// Caches compiled pipeline state for faster subsequent creation.
    class PipelineCache
    {
    public:
        virtual ~PipelineCache() = default;

        [[nodiscard]] virtual u32 GetDataSize() = 0;
        virtual Status GetData(Span<u8> outData) = 0;
    };

    /// Compiled graphics (rasterization) pipeline.
    class RenderPipeline
    {
    public:
        virtual ~RenderPipeline() = default;
        PipelineLayout* layout = nullptr;
    };

    /// Compiled compute pipeline.
    class ComputePipeline
    {
    public:
        virtual ~ComputePipeline() = default;
        PipelineLayout* layout = nullptr;
    };

    /// Compiled mesh shader pipeline.
    class MeshPipeline
    {
    public:
        virtual ~MeshPipeline() = default;
        PipelineLayout* layout = nullptr;
    };

    /// Bottom-level or top-level acceleration structure for ray tracing.
    class AccelStruct
    {
    public:
        virtual ~AccelStruct() = default;

        [[nodiscard]] virtual AccelStructType Type() const = 0;
        [[nodiscard]] virtual u64 DeviceAddress() const = 0;
    };

    /// Compiled ray tracing pipeline.
    class RayTracingPipeline
    {
    public:
        virtual ~RayTracingPipeline() = default;
        PipelineLayout* layout = nullptr;
    };

} // namespace draconic::rhi
