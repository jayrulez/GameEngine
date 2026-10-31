// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Abstract resource classes. Each represents a GPU-allocated object
/// owned and destroyed by the Device.

export module foundation.rhi:resources;

import foundation.core;
import :enums;
import :texture_format;
import :types;
import :descriptors;
import :ext_descriptors;

using namespace foundation::core;

export namespace foundation::rhi
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

        /// Make [offset, offset+size) of a mapped CpuToGpu buffer visible to the GPU. A real
        /// coherent mapping needs nothing (this default); a backend that emulates mapping with
        /// a CPU shadow (WebGPU) uploads exactly that range instead of comparing and re-sending
        /// the whole shadow on Unmap. Contract: a caller that uses FlushRange flushes every
        /// range it wrote during the mapping; Unmap then only closes the mapping. Writers that
        /// touch a small window of a large buffer (the per-frame rings) want this.
        virtual void FlushRange(u64 offset, u64 size)
        {
            (void)offset;
            (void)size;
        }
    };

    /// GPU texture (1D, 2D or 3D, with mip levels and array layers).
    class Texture
    {
    public:
        virtual ~Texture() = default;

        TextureDesc desc{};
        ResourceState initialState = ResourceState::Undefined;
    };

    /// Next monotonic TextureView::uniqueId (thread-safe; ids never repeat). Defined in
    /// ResourcesImpl.cpp so the <atomic> it needs stays out of this interface unit, where it
    /// would make GCC emit an unreadable gcm cluster for consumers.
    [[nodiscard]] u64 NextTextureViewUniqueId() noexcept;

    /// A view into a subset of a texture's mip levels and array layers.
    class TextureView
    {
    public:
        virtual ~TextureView() = default;

        TextureViewDesc desc{};
        Texture* texture = nullptr;
        // Monotonic creation stamp. A destroyed view's address can be reused by the very next
        // allocation, so any cache keyed by TextureView* must validate this id on a hit;
        // trusting the raw pointer can hand out descriptors of a destroyed view, and sampling a
        // dead image view loses the device.
        u64 uniqueId = NextTextureViewUniqueId();
    };

    /// Texture sampler (filtering, addressing, comparison).
    class Sampler
    {
    public:
        virtual ~Sampler() = default;
        SamplerDesc desc{};
    };

    /// Compiled shader module (SPIR-V or DXIL bytecode, or WGSL text).
    class ShaderModule
    {
    public:
        virtual ~ShaderModule() = default;
        /// The entry point the module was compiled for (ShaderModuleDesc::entryPoint); empty for a
        /// module whose stages name their entries. Set by the device that creates it.
        String entryPoint;
    };

    /// A stage that runs its module's own entry point (the one the module was compiled for).
    [[nodiscard]] inline ProgrammableStage StageOf(ShaderModule* module, ShaderStage stage) noexcept
    {
        return ProgrammableStage{module, StringView{}, stage};
    }

    /// The entry point a stage runs: the one it names, else its module's own. Empty when neither
    /// says, which a backend refuses (a module of several entries needs the stage to choose).
    [[nodiscard]] inline StringView EntryPointOf(const ProgrammableStage& stage) noexcept
    {
        if (!stage.entryPoint.IsEmpty())
        {
            return stage.entryPoint;
        }
        return stage.module != nullptr ? stage.module->entryPoint.AsView() : StringView{};
    }

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
    /// layouts and push constant ranges).
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

} // namespace foundation::rhi
