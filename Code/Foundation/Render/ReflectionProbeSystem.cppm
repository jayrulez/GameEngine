// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Foundation::Render - the `:probes` partition.
///
/// Reflection probes: local, parallax-corrected, cluster-assigned cubemap reflections.
/// This system owns the per-probe GPU resources and drives capture, prefilter and froxel assignment:
///   - captured cube array (RGBA16F, [maxProbes×6] layers)   : the raw 6-face scene capture per probe.
///   - prefiltered specular cube array (RGBA16F, mip chain)  : GGX split-sum per probe, sampled by the
///                                                             forward (set 0, t8) with parallax.
///   - probe-metadata StructuredBuffer (GpuProbe[])          : center, box, blend, slice; sampled per froxel.
///
/// A stable map from ProbeKey (entity id) to slot keeps a probe's array slice across frames, so only
/// dirty probes re-capture (static caching). This system allocates the resources and the slot
/// assignment and drives capture, prefilter and the debug view.

module;
#include "Core/Prelude.h"

export module foundation.render:probes;

import foundation.core;
import foundation.rhi;
import foundation.rendergraph;
import foundation.shaders;
import foundation.shaders.system;
import :data; // ReflectionProbe, kMaxReflectionProbes, ProbeUpdateMode

using namespace foundation::core;
namespace rendergraph = foundation::rendergraph;
namespace shaders = foundation::shaders;
namespace rhi = foundation::rhi;

export namespace foundation::render
{

    // GPU-side probe record (set-0 t9 StructuredBuffer), 64 bytes. Packed so the forward can, per froxel,
    // pick the probe, box-project the reflection ray, and index its cube-array slice.
    // As lighting.hlsli declares it; Integration.ShaderBlocks checks the two agree.
    struct GpuProbe
    {
        Float3 center;          // the capture centre (world)
        f32 intensity = 1.0f;   // the reflection's strength
        Float3 boxMin;          // the influence box's near corner (world)
        f32 blendDistance = 0;  // the fade at the box's faces
        Float3 boxMax;          // its far corner
        f32 slice = 0;          // the probe's cube in the array (a float, as the sampler takes it)
        f32 mipCount = 0;       // the prefiltered mips
        f32 priority = 0;
        f32 parallax = 0;       // 1: box-project the reflection ray
        f32 _pad = 0;
    };
    static_assert(sizeof(GpuProbe) == 64);

    // The probe prefilter's push block (struct Push, probe_prefilter_ps.ps.hlsl; pad0 and pad1 are its
    // float2 Pad); Integration.ShaderBlocks checks it against the compiled shader, member by member.
    struct PrefilterPush
    {
        i32 faceIndex = 0;
        f32 roughness = 0.0f;
        f32 pad0 = 0.0f, pad1 = 0.0f;
    };
    static_assert(sizeof(PrefilterPush) == 16, "push Push (probe prefilter) layout drift");

    class ReflectionProbeSystem
    {
    public:
        // One fixed array resolution for all probe slices (a cube array can't vary per slice, so the
        // component's per-probe `resolution` is not applied here).
        static constexpr u32 kCaptureRes = 128;
        static constexpr u32 kPrefilterRes = 128;
        static constexpr u32 kPrefilterMips = 5; // roughness = mip / (mips - 1)
        static constexpr u32 kMaxProbes = kMaxReflectionProbes;
        static constexpr rhi::TextureFormat kCubeFormat = rhi::TextureFormat::RGBA16Float;

        ReflectionProbeSystem(rhi::Device& device, shaders::ShaderSystem& shaders) noexcept
            : m_device(&device), m_shaders(&shaders)
        {
        }

        ~ReflectionProbeSystem() { DestroyResources(); }

        Status Initialize();

        // Reset the frame's record accumulation (records are per-scene ranges appended by Assign;
        // slot and capture state persist across frames). Call once per frame before any Assign.
        void BeginFrame();

        // Map one scene's extracted probes onto persistent array slots (stable per ProbeKey) and append
        // their CPU-side GpuProbe records to this frame's buffer. Called once per extracted scene per
        // frame (frames can render several scenes side by side; each view later reads its own scene's
        // record range, see RangeFor). Re-assigning a scene already seen this frame is a no-op (several
        // views of one scene). Marks a probe dirty (needs capture) when it takes a new slot or its transform
        // changed since last assign. Probes beyond kMaxProbes are dropped (logged by the caller if it
        // cares).
        u32 Assign(const ExtractedScene* scene, Span<const ReflectionProbe> probes);

        // This frame's record range for a scene (views offset the shader's probe loop by it).
        struct ProbeRange
        {
            u32 base = 0;
            u32 count = 0;
        };
        [[nodiscard]] ProbeRange RangeFor(const ExtractedScene* scene) const noexcept;

        [[nodiscard]] u32 ActiveCount() const noexcept { return m_active; }
        [[nodiscard]] Span<const GpuProbe> CpuProbes() const noexcept;

        // Upload this frame's probe records into the metadata buffer (set-0 t9). Call once per frame after Assign.
        void Upload();

        // A probe that needs (re)capture this frame: its array slot, world capture center and the scene the
        // capture must render (its owning scene's geometry, in multi-scene frames). The capture loop renders
        // into layers [LayerBase(slot) .. +6) then calls MarkCaptured(slot).
        struct CaptureTask
        {
            u32 slot;
            Float3 center;
            const ExtractedScene* scene = nullptr;
        };
        [[nodiscard]] Span<const CaptureTask> Captures() const noexcept;
        [[nodiscard]] static u32 LayerBase(u32 slot) noexcept { return slot * 6u; }
        void MarkCaptured(u32 slot) noexcept;

        // Near and far planes for the 90° FOV face cameras (cover the box interior out to distant geometry and sky).
        static constexpr f32 kCaptureNear = 0.1f;
        static constexpr f32 kCaptureFar = 1000.0f;

        // Transition the whole captured cube array to ShaderRead once (out of graph, at first encoder hold),
        // so uncaptured slices aren't left UNDEFINED when the forward binds the whole-array SRV (same reason
        // the mesh renderer's dummy textures are pre-transitioned). Idempotent.
        void InitLayouts(rhi::CommandEncoder& encoder);

        // Import the captured cube array into the graph (whole resource; the capture passes target individual
        // layers via subresource ranges). Persists its resource state across frames like the shadow atlas.
        rendergraph::RGHandle ImportCaptured(rendergraph::RenderGraph& graph);

        // Import the prefiltered cube array (the separate texture the forward samples at t8, never a capture
        // render target, so no read/write hazard with the capture passes; captured is copied into it below).
        rendergraph::RGHandle ImportPrefiltered(rendergraph::RenderGraph& graph);

        // Blit captured into prefiltered for one slot's 6 faces (mip 0), correcting the RH-LookAt horizontal
        // mirror with a flip (mip 0 stays sharp, roughness 0; DeclarePrefilter GGX-convolves the rougher
        // mips). One render pass per face samples the captured cube array and writes the prefiltered
        // layer; the graph orders the capture write, then the blit read and write, then the forward read.
        void DeclareBlit(rendergraph::RenderGraph& graph, rendergraph::RGHandle capturedH,
                         rendergraph::RGHandle prefilteredH, u32 slot);

        // GGX-convolve prefiltered mip 0 (the corrected cube) into mips 1..N-1 (roughness = mip/(mips-1)).
        // Reads mip 0 and writes each rougher mip and face; the graph orders it after DeclareBlit (which wrote
        // mip 0) through the shared prefilteredH. The forward samples roughness*(mips-1), blurrier at higher
        // roughness.
        void DeclarePrefilter(rendergraph::RenderGraph& graph, rendergraph::RGHandle prefilteredH,
                              u32 slot);

        // Resources (consumed by the forward, and by capture and prefilter).
        // The captured cube array as a sample view: the whole-array view used for the render-target import, a
        // stable single-allocation view. The forward samples PrefilterArrayView (set-0 t8).
        [[nodiscard]] rhi::TextureView* CapturedSampleView() const noexcept;
        [[nodiscard]] rhi::TextureView* PrefilterArrayView() const noexcept;
        [[nodiscard]] rhi::Buffer* ProbeBuffer() const noexcept { return m_probeBuffer; }
        [[nodiscard]] rhi::Sampler* Sampler() const noexcept { return m_sampler; }

    private:
        static constexpr u32 kInvalidSlot = 0xFFFFFFFFu;

        struct SlotState
        {
            u64 probeKey = 0;
            u64 signature = 0;
            bool captured = false; // a full cube has been captured and prefiltered at least once
            bool dirty = false;    // needs (re)capture this frame
            u32 captureRequest = 0; // the probe's request count when last seen
        };

        // Stable slot for a probe key: reuse if seen, else claim the next free slot (up to kMaxProbes).
        u32 SlotFor(u64 key);

        // Cheap change signature over a probe's captured-input state (transform, box, resolution). A bit-mixed
        // hash of the floats; exact equality is fine here (it only needs to tell a change since last frame).
        [[nodiscard]] static u64 TransformSignature(const ReflectionProbe& p);

        bool CreateResources();

        bool CreateBlitPipeline();

        // Lazily create (and cache) a probe slot and face's captured 2D-layer SRV and its blit bind group.
        // Sampling the single face layer as a Texture2D (not the cube) keeps filtering inside the face, so
        // there are no seams.
        rhi::BindGroup* EnsureFaceBlit(u32 slot, u32 face);

        bool CreatePrefilterPipeline();

        // Per-slot source cube view (prefiltered mip 0 as a TextureCube) and its prefilter bind group.
        rhi::BindGroup* EnsurePrefilterSource(u32 slot);

        void DestroyResources();

        rhi::Device* m_device = nullptr;
        shaders::ShaderSystem* m_shaders = nullptr;

        // Captured-to-prefiltered flip blit (corrects the RH-LookAt mirror). Per-face 2D SRVs and bind groups so
        // filtering stays within a face (no cross-face seams in smooth gradients like the sky).
        rhi::BindGroupLayout* m_blitLayout = nullptr;
        rhi::PipelineLayout* m_blitPipeLayout = nullptr;
        rhi::RenderPipeline* m_blitPipeline = nullptr;
        rhi::TextureView* m_capturedFaceView[kMaxProbes * 6] = {};
        rhi::BindGroup* m_blitFaceBG[kMaxProbes * 6] = {};

        // GGX roughness prefilter (prefiltered mip 0 into mips 1..N-1).
        rhi::BindGroupLayout* m_prefilterLayout = nullptr;
        rhi::PipelineLayout* m_prefilterPipeLayout = nullptr;
        u64 m_pipelineShaderVersion = 0; // ShaderSystem::Version at build (hot reload)
        rhi::RenderPipeline* m_prefilterPipeline = nullptr;
        rhi::TextureView* m_prefilterSrcView[kMaxProbes] =
            {}; // per-slot mip-0 cube view (convolution source)
        rhi::BindGroup* m_prefilterSrcBG[kMaxProbes] = {};

        rhi::Texture* m_capturedCube = nullptr;
        rhi::TextureView* m_capturedArrayView = nullptr;
        rhi::ResourceState m_capturedState =
            rhi::ResourceState::Undefined; // persists across frames (import)
        bool m_layoutsInit = false;        // one-time whole-array ShaderRead init
        rhi::Texture* m_prefilterCube = nullptr;
        rhi::TextureView* m_prefilterArrayView = nullptr;
        rhi::ResourceState m_prefilterState =
            rhi::ResourceState::Undefined; // persists across frames (import)
        rhi::Buffer* m_probeBuffer = nullptr;
        rhi::Sampler* m_sampler = nullptr;

        Array<CaptureTask> m_captures; // dirty probes to (re)capture this frame

        // This frame's per-scene record ranges (Assign appends in scene order; views read RangeFor).
        struct SceneRange
        {
            const ExtractedScene* scene = nullptr;
            u32 base = 0;
            u32 count = 0;
        };
        Array<SceneRange> m_sceneRanges;

        HashMap<u64, u32> m_slots; // ProbeKey -> array slot (persistent)
        u32 m_nextSlot = 0;
        SlotState m_slotState[kMaxProbes];
        GpuProbe m_cpuProbes[kMaxProbes];
        u32 m_active = 0;
        // Startup re-capture window: keep probes dirty for the first frames so a dropped web
        // startup submit cannot silently lose the one-shot bake (like IBLSystem's m_bakeWarmup).
        u32 m_captureWarmup = 20;
    };

} // namespace foundation::render
