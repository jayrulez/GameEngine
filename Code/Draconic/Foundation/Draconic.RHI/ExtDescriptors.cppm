/// Descriptor structs for mesh shader and ray tracing extensions.

module;

export module draconic.rhi:ext_descriptors;

import draconic.foundation;
import :enums;
import :texture_format;
import :types;
import :forward;
import :descriptors;

using namespace draconic::foundation;

export namespace draconic::rhi
{

    // ---- Mesh shader pipeline ----

    /// Descriptor for creating a mesh shader pipeline.
    struct MeshPipelineDesc
    {
        PipelineLayout* layout = nullptr;
        Optional<ProgrammableStage> task; ///< Optional task (amplification) shader.
        ProgrammableStage mesh;           ///< Required mesh shader.
        Optional<FragmentState> fragment;
        Span<const ColorTargetState> colorTargets;
        PrimitiveState primitive;
        Optional<DepthStencilState> depthStencil;
        MultisampleState multisample;
        PipelineCache* cache = nullptr;
        StringView label;
    };

    // ---- Ray tracing ----

    /// Descriptor for creating an acceleration structure.
    struct AccelStructDesc
    {
        AccelStructType type = AccelStructType::BottomLevel;
        AccelStructBuildFlags flags = AccelStructBuildFlags::PreferFastTrace;
        StringView label;
    };

    /// Triangle geometry for BLAS construction.
    struct AccelStructGeometryTriangles
    {
        Buffer* vertexBuffer = nullptr;
        u64 vertexOffset = 0;
        u32 vertexCount = 0;
        u32 vertexStride = 0;
        VertexFormat vertexFormat = VertexFormat::Float32x3;
        Buffer* indexBuffer = nullptr;
        u64 indexOffset = 0;
        u32 indexCount = 0;
        IndexFormat indexFormat = IndexFormat::UInt32;
        Buffer* transformBuffer = nullptr;
        u64 transformOffset = 0;
        GeometryFlags flags = GeometryFlags::Opaque;
    };

    /// AABB geometry for procedural BLAS construction.
    struct AccelStructGeometryAABBs
    {
        Buffer* aabbBuffer = nullptr;
        u64 offset = 0;
        u32 count = 0;
        u32 stride = 24; ///< sizeof(VkAabbPositionsKHR)
        GeometryFlags flags = GeometryFlags::Opaque;
    };

    /// Shader group definition for a ray tracing pipeline.
    struct RayTracingShaderGroup
    {
        enum class Type : u32
        {
            General,
            TrianglesHitGroup,
            ProceduralHitGroup
        };

        Type type = Type::General;
        u32 generalShaderIndex = ~0u;
        u32 closestHitShaderIndex = ~0u;
        u32 anyHitShaderIndex = ~0u;
        u32 intersectionShaderIndex = ~0u;
    };

    /// Descriptor for creating a ray tracing pipeline.
    struct RayTracingPipelineDesc
    {
        PipelineLayout* layout = nullptr;
        Span<const ProgrammableStage> stages;
        Span<const RayTracingShaderGroup> groups;
        u32 maxRecursionDepth = 1;
        u32 maxPayloadSize = 0;
        u32 maxAttributeSize = 0;
        PipelineCache* cache = nullptr;
        StringView label;
    };

} // namespace draconic::rhi
