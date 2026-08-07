/// Draconic::Render - the `:decal_pass` partition.
///
/// Screen-space projected decals, ported from SedulousEngine's DecalPass/decal.frag: reconstruct the
/// world position under each pixel from the scene depth, transform it into a decal's oriented unit box,
/// clip to the box, and alpha-blend the decal texture onto the lit HDR - a "sprayed" sticker that lands
/// on whatever surface is under the box (including animated meshes, since it reads the depth buffer).
///
/// Runs after the forward+sky pass and BEFORE AO/TAA, blending into the HDR (so decals get TAA-resolved).
/// Draconic matches Sedulous's shader assumptions (row-major, D3D-style [0,1] clip depth, top-origin uv
/// under the negative viewport - same reconstruction as AoPass), so the projection math ports verbatim.
///
/// v1 draws a FULLSCREEN triangle per decal (robust across split-screen sub-rects + no box winding/cull
/// pitfalls); the box test lives in the fragment shader. Receiver normal for angle-fade comes from the
/// reconstructed world-position derivatives (ddx/ddy), faithful to the port. (A per-decal box mesh +
/// sampling the normalT G-buffer are later optimizations.)

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.render:decal_pass;

import draconic.foundation;
import draconic.rhi;
import draconic.rendergraph;
import draconic.shaders;
import draconic.shaders.system;
import :data;
import :resources;

using namespace draconic::foundation;
namespace rhi = draconic::rhi;

export namespace draconic::render
{

    // The scene HDR format decals blend into (matches TonemapPass::HdrFormat / AoPass).
    inline constexpr rhi::TextureFormat kDecalHdrFormat = rhi::TextureFormat::RGBA16Float;

    // One decal, uploaded to the per-decal UBO (set 1, dynamic offset). 224 bytes.
    struct DecalUniforms
    {
        Float4x4 world;    // decal box world transform (scale = box size); projects along local +Z
        Float4x4 invWorld; // world -> decal-local (the box clip)
        Float4x4
            invViewProj; // (ndc, depth) -> world, for depth reconstruction (this view, jittered)
        Float4 color;    // rgba tint
        Float4 params;   // x,y = 1/full-target-size ; z,w = cos(angleFadeStart), cos(angleFadeEnd)
        Float4 flip;     // x = interpolant Y correction (+1 Vulkan, -1 Y-flip targets); yzw spare
    };
    static_assert(sizeof(DecalUniforms) == 240);

    class DecalPass
    {
    public:
        DecalPass(rhi::Device& device, shaders::ShaderSystem& shaders, u32 framesInFlight) noexcept
            : m_device(&device), m_shaders(&shaders),
              m_decalRing(device, framesInFlight, sizeof(DecalUniforms) <= 256 ? 256u : 512u,
                          rhi::BufferUsage::Uniform | rhi::BufferUsage::CopyDst, u8"decal.uniforms")
        {
        }
        ~DecalPass() { Shutdown(); }
        DecalPass(const DecalPass&) = delete;
        DecalPass& operator=(const DecalPass&) = delete;

        Status Initialize();

        // Bracket the frame ONCE (before the per-view loop). The per-decal uniform ring must NOT be reset per
        // view - DeclareDecals is called once per view, and the GPU reads the ring at graph-execute time
        // (after ALL views have recorded), so per-view resets would make every view read the last view's
        // slot (its InvViewProj) -> decals reconstruct with the wrong matrix and swim / couple across views.
        void BeginFrame(u32 frameIndex);
        void EndFrame() { m_decalRing.EndFrame(); }

        // Blend the scene's decals into `hdr` for one view. `viewProj` is the view's (jittered) view-proj,
        // whose inverse reconstructs world position from `depth`. Allocates its own ring slots (accumulating
        // across views within the frame). No-op if there are no decals. Bracket with BeginFrame/EndFrame.
        void DeclareDecals(rendergraph::RenderGraph& graph, rendergraph::RGHandle hdr,
                           rendergraph::RGHandle depth, Span<const DecalInstance> decals,
                           const Float4x4& viewProj, u32 w, u32 h, i32 vpX, i32 vpY, u32 vpW,
                           u32 vpH);

        /// Wire the frames-in-flight retire queue (web-safe ring grows). Null = drain.
        void SetRetireQueue(GpuRetireQueue* retire) noexcept
        {
            m_decalRing.SetRetireQueue(retire);
        }

    private:
        struct Draw
        {
            u32 offset;
            rhi::TextureView* texture;
        };

        static constexpr u32 kRetireFrames = 3;
        static constexpr u32 kMaxDecalsPerFrame =
            256; // ring capacity (across all views) - reserved once

        rhi::RenderPipeline* MakePipeline();

        // One bind group over the decal-uniform ring (dynamic offset per decal). Rebuilt only on ring
        // realloc (generation), which drains the GPU first - never freeing an in-flight set.
        rhi::BindGroup* EnsureUboBindGroup();

        // Depth bind group (set 0), cached by (view, generation) with a defer-free retire list - the depth
        // transient is aliased/recreated, so a raw-pointer cache would free a set an in-flight frame still uses.
        rhi::BindGroup* EnsureDepthBindGroup(rhi::TextureView* depth, u64 generation);

        rhi::BindGroup* EnsureTextureBindGroup(rhi::TextureView* tex);

        void Tick(u32 frameIndex);

        void Shutdown();

        struct Retired
        {
            rhi::BindGroup* bg = nullptr;
            u32 left = 0;
        };

        rhi::Device* m_device;
        shaders::ShaderSystem* m_shaders;
        DynamicUniformRing m_decalRing;
        rhi::BindGroupLayout* m_depthLayout = nullptr;
        rhi::BindGroupLayout* m_uboLayout = nullptr;
        rhi::BindGroupLayout* m_texLayout = nullptr;
        rhi::PipelineLayout* m_pipelineLayout = nullptr;
        rhi::RenderPipeline* m_pipeline = nullptr;
        u64 m_pipelineShaderVersion = 0; // ShaderSystem::Version at build (hot reload)
        rhi::Sampler* m_depthSampler = nullptr;
        rhi::Sampler* m_texSampler = nullptr;
        rhi::BindGroup* m_uboBg = nullptr;
        u32 m_uboBgGen = 0xFFFFFFFFu;
        rhi::BindGroup* m_depthBg = nullptr;
        rhi::TextureView* m_depthView = nullptr;
        u64 m_depthGen = 0;
        u32 m_lastFrame = 0xFFFFFFFFu;
        bool m_reserved = false;
        Array<Retired> m_retired;
        // Keyed by view pointer, validated by TextureView::uniqueId on every hit (address
        // reuse of destroyed dynamic textures - see SpriteRenderer::TexBindGroup).
        struct TexBindGroup
        {
            rhi::BindGroup* bindGroup = nullptr;
            u64 viewId = 0;
        };
        HashMap<rhi::TextureView*, TexBindGroup> m_texBindGroups;
    };

} // namespace draconic::render
