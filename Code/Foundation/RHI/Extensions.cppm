// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Extension base classes for multiple inheritance.
///
/// Backend encoders that support an extension inherit it alongside their core base class:
///   class VkRenderPassEncoder : public RenderPassEncoder, public MeshShaderPassExt { ... };
///   class VkCommandEncoder : public CommandEncoder, public RayTracingEncoderExt { ... };
///
/// The engine builds without RTTI, so callers probe for support through the cross-queries on the
/// core classes rather than a dynamic_cast:
///   if (auto* mp = renderPass->AsMeshShaderExt()) { ... }
///   if (auto* rt = encoder->AsRayTracingExt()) { ... }

export module foundation.rhi:extensions;

import foundation.core;
import :enums;
import :types;
import :descriptors;
import :ext_descriptors;
import :resources;

using namespace foundation::core;

export namespace foundation::rhi
{

    /// Mesh shader render pass extension. Implemented by backend
    /// RenderPassEncoder subclasses that support mesh shaders.
    class MeshShaderPassExt
    {
    public:
        virtual ~MeshShaderPassExt() = default;

        /// Bind a mesh shader pipeline.
        virtual void SetMeshPipeline(MeshPipeline* pipeline) = 0;
        /// Dispatch mesh shader work groups.
        virtual void DrawMeshTasks(u32 groupCountX, u32 groupCountY = 1, u32 groupCountZ = 1) = 0;
        /// Dispatch mesh shader work groups via an indirect buffer.
        virtual void DrawMeshTasksIndirect(Buffer* buffer, u64 offset, u32 drawCount = 1,
                                           u32 stride = 0) = 0;
        /// Dispatch mesh shader work groups with an indirect count buffer.
        virtual void DrawMeshTasksIndirectCount(Buffer* buffer, u64 offset, Buffer* countBuffer,
                                                u64 countOffset, u32 maxDrawCount, u32 stride) = 0;
    };

    /// Ray tracing command encoder extension. Implemented by backend
    /// CommandEncoder subclasses that support ray tracing.
    class RayTracingEncoderExt
    {
    public:
        virtual ~RayTracingEncoderExt() = default;

        /// Build a bottom-level acceleration structure from triangle or AABB geometry, or both.
        virtual void BuildBottomLevelAccelStruct(AccelStruct* dst, Buffer* scratchBuffer,
                                                 u64 scratchOffset,
                                                 Span<const AccelStructGeometryTriangles> triangles,
                                                 Span<const AccelStructGeometryAABBs> aabbs) = 0;

        /// Build a top-level acceleration structure from an instance buffer.
        virtual void BuildTopLevelAccelStruct(AccelStruct* dst, Buffer* scratchBuffer,
                                              u64 scratchOffset, Buffer* instanceBuffer,
                                              u64 instanceOffset, u32 instanceCount) = 0;

        /// Bind a ray tracing pipeline.
        virtual void SetRayTracingPipeline(RayTracingPipeline* pipeline) = 0;

        /// Bind a resource group for the ray tracing pipeline.
        virtual void SetBindGroup(u32 index, BindGroup* group,
                                  Span<const u32> dynamicOffsets = {}) = 0;

        /// Upload push constants for the ray tracing pipeline.
        virtual void SetPushConstants(ShaderStage stages, u32 offset, u32 size,
                                      const void* data) = 0;

        /// Dispatch rays using shader binding tables.
        virtual void TraceRays(Buffer* raygenSBT, u64 raygenOffset, u64 raygenStride,
                               Buffer* missSBT, u64 missOffset, u64 missStride, Buffer* hitSBT,
                               u64 hitOffset, u64 hitStride, u32 width, u32 height,
                               u32 depth = 1) = 0;
    };

} // namespace foundation::rhi
