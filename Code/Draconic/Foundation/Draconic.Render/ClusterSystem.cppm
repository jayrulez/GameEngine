/// Draconic::Render - the `:cluster_system` partition.
///
/// Clustered light culling (phase 4.3). Bins lights into a 3D froxel grid (screen tiles ×
/// logarithmic depth slices) once per frame via a compute pass, so the forward shader evaluates
/// only the lights touching each fragment's cluster instead of all lights. This partition owns the
/// build compute pipeline + the per-frame cluster buffers, and declares the build pass into the
/// frame graph (ordered before the forward pass, which reads the buffers it writes).
///
/// 4.3a establishes the plumbing with a STUB kernel (writes empty cluster lists); 4.3b ports the
/// real froxel-AABB / sphere-assignment kernel; 4.3c wires the buffers into the forward shader.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.render:cluster_system;

import draconic.foundation;
import draconic.rhi;
import draconic.rendergraph;
import draconic.shaders;
import draconic.shaders.system;
import :data;
import :views;
import :resources;

using namespace draconic::foundation;
namespace rhi = draconic::rhi;

export namespace draconic::render
{

    // What a built cluster grid exposes to the forward pass (the buffers + the grid params the
    // fragment shader needs to map a pixel+depth to its cluster). Filled by DeclareBuild.
    struct ClusterBinding
    {
        rhi::Buffer* offsets = nullptr;      // uint2 per cluster: (indexStart, count)
        rhi::Buffer* lightIndices = nullptr; // flat uint light-index list
        // Bumped every time this slot's buffers are reallocated (resize). Consumers must invalidate any
        // cached bind group on a version change - a freed rhi::Buffer* address can be REUSED by the new
        // allocation, so pointer-equality is NOT a reliable "unchanged" test (use-after-free otherwise).
        u32 version = 0;
        rendergraph::RGHandle offsetsHandle =
            {}; // graph handles so the forward pass can ReadBuffer them
        rendergraph::RGHandle indicesHandle =
            {}; // (orders the build write before the shading read)
        u32 gridX = 0, gridY = 0, sliceCount = 0, tileSize = 0;
        i32 viewportX = 0,
            viewportY = 0; // grid is viewport-local; forward subtracts this from SV_Position
        f32 nearZ = 0.0f, farZ = 0.0f, logScale = 0.0f, logBias = 0.0f;
        [[nodiscard]] bool Valid() const noexcept
        {
            return offsets != nullptr && lightIndices != nullptr;
        }
    };

    class ClusterSystem
    {
    public:
        ClusterSystem(rhi::Device& device, shaders::ShaderSystem& shaders,
                      u32 framesInFlight) noexcept
            : m_device(&device), m_shaders(&shaders),
              m_framesInFlight(framesInFlight < 1 ? 1 : framesInFlight),
              m_paramsRing(device, m_framesInFlight, kParamsSlot,
                           rhi::BufferUsage::Uniform | rhi::BufferUsage::CopyDst,
                           u8"cluster.params"),
              m_lightRing(device, m_framesInFlight, sizeof(GpuLight),
                          rhi::BufferUsage::StorageRead | rhi::BufferUsage::CopyDst, u8"cluster.lights")
        {
        }

        ~ClusterSystem() { Shutdown(); }
        ClusterSystem(const ClusterSystem&) = delete;
        ClusterSystem& operator=(const ClusterSystem&) = delete;

        // Build the compute pipeline + its layout. Returns non-Ok if the shader/pipeline fails.
        Status Initialize();

        // Size the params ring for this frame + select its region. Call once per frame before any view.
        void PrepareFrame(u32 frameIndex);

        // Declare this view's cluster build compute pass into the graph (ordered before the forward
        // pass that reads the cluster buffers). Returns the binding the forward pass consumes, or an
        // empty binding if unavailable. The cluster grid is derived from the view's resolution + camera.
        ClusterBinding DeclareBuild(rendergraph::RenderGraph& graph, const RenderView& view,
                                    u32 frameIndex, u32 viewIndex);

        /// Wire the frames-in-flight retire queue (web-safe buffer grows). Null = drain.
        void SetRetireQueue(GpuRetireQueue* retire) noexcept
        {
            m_retire = retire;
            m_paramsRing.SetRetireQueue(retire);
            m_lightRing.SetRetireQueue(retire);
        }

    private:
        GpuRetireQueue* m_retire = nullptr; // borrowed; null = WaitIdle on grow
        // Matches the ClusterBuildParams cbuffer (std140; 16-aligned scalars + two row-major mats).
        struct BuildParams
        {
            u32 gridX = 0, gridY = 0, sliceCount = 0, tileSize = 0;
            f32 nearZ = 0.0f, farZ = 0.0f, logScale = 0.0f, logBias = 0.0f;
            u32 lightCount = 0, lightOffset = 0;
            f32 pad0 = 0.0f, pad1 = 0.0f;
            Float4x4 viewMatrix;
            Float4x4 invProjection;
        };

        static constexpr u32 kTileSize = 16;
        static constexpr u32 kSliceCount = 24;
        static constexpr u32 kMaxPerCluster =
            64; // per-cluster light cap (matches MAX_PER_CLUSTER in the kernel)
        static constexpr u32 kMaxLights = 256; // per-view light budget (matches the forward's)
        static constexpr u64 kParamsSlot =
            256; // dynamic UBO offset alignment (>= sizeof(BuildParams))
        static constexpr u32 kMaxViewsPerFrame = 8;

        // (Re)create one (view,frame) slot's cluster buffers when its cluster count grows. Lazy: only
        // the slots actually used by rendered views are allocated.
        bool EnsureBuffers(u32 bufferSlot, u32 totalClusters);

        // (Re)build the bind group for a (view,frame) slot. One bind group PER slot (each over its own
        // buffers) so a slot's group is stable across frames - never freed while the previous frame's
        // command buffer that referenced it is still in flight.
        bool EnsureBindGroup(u32 bufferSlot, rhi::Buffer* offsets, rhi::Buffer* indices);

        void Shutdown();

        rhi::Device* m_device;
        shaders::ShaderSystem* m_shaders;
        u32 m_framesInFlight = 2;

        rhi::BindGroupLayout* m_layout = nullptr;
        rhi::PipelineLayout* m_pipelineLayout = nullptr;
        rhi::ComputePipeline* m_pipeline = nullptr;
        u64 m_pipelineShaderVersion = 0; // ShaderSystem::Version at build (hot reload)

        static constexpr u32 kMaxFramesInFlight = 8;
        // Cluster buffers are owned per (view, frame-in-flight) slot - two views in one frame must not
        // share a buffer (the Sedulous "two pipelines stomp the same frameIndex%2 slot" bug). Slots are
        // allocated LAZILY (the indices buffer is large), so unused view slots cost only a null pointer.
        static constexpr u32 kMaxBufferSlots = kMaxViewsPerFrame * kMaxFramesInFlight;

        DynamicUniformRing m_paramsRing;
        DynamicUniformRing
            m_lightRing; // ClusterSystem's own light copy (built before the forward uploads its)
        rhi::Buffer* m_offsets[kMaxBufferSlots] = {}; // per-(view,frame) cluster (offset,count)
        rhi::Buffer* m_indices[kMaxBufferSlots] = {}; // per-(view,frame) flat light-index list
        u64 m_offsetsBytes[kMaxBufferSlots] = {}; // size of each slot's buffers (views can differ)
        u64 m_indicesBytes[kMaxBufferSlots] = {};
        u32 m_bufferVersion[kMaxBufferSlots] =
            {}; // ++ on realloc; consumers invalidate cached bind groups

        // One bind group per (view, frame) slot (each over its own buffers), so a slot's group is never
        // freed while still referenced by an in-flight frame.
        rhi::BindGroup* m_bindGroups[kMaxBufferSlots] = {};
        u32 m_bgParamsGen[kMaxBufferSlots] = {};
        u32 m_bgLightGen[kMaxBufferSlots] = {};
        rhi::Buffer* m_bgOffsets[kMaxBufferSlots] = {};
        bool m_ready = false;
    };

} // namespace draconic::render
