/// Draconic::Render - the `:probes` partition.
///
/// Reflection probes: local, parallax-corrected, cluster-assigned cubemap reflections
/// (docs/design/reflection-probes.md). This system owns the per-probe GPU resources and (in later
/// sub-phases) drives capture + prefilter + froxel assignment:
///   - captured cube-ARRAY (RGBA16F, [maxProbes×6] layers)   : the raw 6-face scene capture per probe.
///   - prefiltered specular cube-ARRAY (RGBA16F, mip chain)  : GGX split-sum per probe, sampled by the
///                                                             forward (set 0, t8) with parallax.
///   - probe-metadata StructuredBuffer (GpuProbe[])          : center/box/blend/slice, sampled per froxel.
///
/// A stable ProbeKey (entity id) → slot map keeps a probe's array slice persistent across frames, so only
/// dirty probes re-capture (static caching). This sub-phase (P1a) allocates the resources + the slot
/// assignment; capture, prefilter, and the debug view land in P1b/P1c.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.render:probes;

import draconic.foundation;
import draconic.rhi;
import draconic.rendergraph;
import draconic.shaders;
import draconic.shaders.system;
import :data; // ReflectionProbe / kMaxReflectionProbes / ProbeUpdateMode

using namespace draconic::foundation;
namespace rhi = draconic::rhi;

export namespace draconic::render
{

    // GPU-side probe record (set-0 t9 StructuredBuffer), 64 bytes. Packed so the forward can, per froxel,
    // pick the probe, box-project the reflection ray, and index its cube-array slice.
    struct GpuProbe
    {
        Float4 center; // xyz = capture center (world),          w = intensity
        Float4 boxMin; // xyz = box min corner (world),          w = blendDistance
        Float4
            boxMax; // xyz = box max corner (world),          w = sliceBase (float; cube index into the array)
        Float4 params; // x = mipCount, y = priority,            zw = pad
    };
    static_assert(sizeof(GpuProbe) == 64);

    struct PrefilterPush
    {
        i32 faceIndex = 0;
        f32 roughness = 0.0f;
        f32 pad0 = 0.0f, pad1 = 0.0f;
    };
    static_assert(sizeof(PrefilterPush) == 16);

    class ReflectionProbeSystem
    {
    public:
        // One fixed array resolution for all probe slices (a cube-array can't vary per-slice; the component's
        // per-probe `resolution` is honored later via separate textures if needed).
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
        // slot/capture state persists across frames). Call once per frame before any Assign.
        void BeginFrame();

        // Map ONE SCENE's extracted probes onto persistent array slots (stable per ProbeKey) and append
        // their CPU-side GpuProbe records to this frame's buffer. Called once per extracted scene per
        // frame (frames can render several scenes side-by-side; each view later reads ITS scene's record
        // range - see RangeFor). Re-assigning a scene already seen this frame is a no-op (same-scene
        // multi-view). Marks a probe dirty (needs capture) when it takes a NEW slot or its transform
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

        // A probe that needs (re)capture this frame: its array slot + world capture center + the scene the
        // capture must render (ITS owning scene's geometry - multi-scene frames). The capture loop renders
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

        // Near/far planes for the 90°-FOV face cameras (cover the box interior out to distant geometry + sky).
        static constexpr f32 kCaptureNear = 0.1f;
        static constexpr f32 kCaptureFar = 1000.0f;

        // Transition the WHOLE captured cube-array to ShaderRead once (out of graph, at first encoder hold),
        // so uncaptured slices aren't left UNDEFINED when the forward binds the whole-array SRV (same reason
        // the mesh renderer's dummy textures are pre-transitioned). Idempotent.
        void InitLayouts(rhi::CommandEncoder& encoder);

        // Import the captured cube-array into the graph (whole resource; the capture passes target individual
        // layers via subresource ranges). Persists its resource state across frames like the shadow atlas.
        rendergraph::RGHandle ImportCaptured(rendergraph::RenderGraph& graph);

        // Import the prefiltered cube-array (the SEPARATE texture the forward samples at t8 - never a capture
        // render target, so no read/write hazard with the capture passes; captured is copied into it below).
        rendergraph::RGHandle ImportPrefiltered(rendergraph::RenderGraph& graph);

        // Bridge captured -> prefiltered for one slot's 6 faces (mip 0), correcting the RH-LookAt horizontal
        // mirror via a flip blit (sharp for now; a GGX roughness convolution replaces this later). One render
        // pass per face samples the captured cube-array and writes the prefiltered layer; the graph orders
        // capture-write -> blit-read + blit-write -> forward-read.
        void DeclareBlit(rendergraph::RenderGraph& graph, rendergraph::RGHandle capturedH,
                         rendergraph::RGHandle prefilteredH, u32 slot);

        // GGX-convolve prefiltered mip 0 (the corrected cube) into mips 1..N-1 (roughness = mip/(mips-1)).
        // Reads mip 0, writes each rougher mip/face; the graph orders after DeclareBlit (which wrote mip 0)
        // via the shared prefilteredH. The forward samples roughness*(mips-1) -> blurrier at higher roughness.
        void DeclarePrefilter(rendergraph::RenderGraph& graph, rendergraph::RGHandle prefilteredH,
                              u32 slot);

        // Resources (consumed by the forward in P2, and by capture/prefilter in P1b/c).
        // The captured cube-ARRAY as a sample view (set-0 t8; P2 samples this directly, sharp mip 0). Reuses
        // the whole-array view used for the render-target import - a stable, single-allocation view.
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
            bool captured = false; // a full cube has been captured + prefiltered at least once
            bool dirty = false;    // needs (re)capture this frame
        };

        // Stable slot for a probe key: reuse if seen, else claim the next free slot (up to kMaxProbes).
        u32 SlotFor(u64 key);

        // Cheap change signature over a probe's captured-input state (transform + box + resolution). Bit-mixed
        // hash of the floats; exact equality is fine here (we only need "changed vs last frame").
        [[nodiscard]] static u64 TransformSignature(const ReflectionProbe& p);

        bool CreateResources();

        bool CreateBlitPipeline();

        // Lazily create (and cache) a probe slot+face's captured 2D-layer SRV + its blit bind group. Sampling
        // the single face layer as a Texture2D (not the cube) keeps filtering inside the face -> no seams.
        rhi::BindGroup* EnsureFaceBlit(u32 slot, u32 face);

        bool CreatePrefilterPipeline();

        // Per-slot source cube view (prefiltered mip 0 as a TextureCube) + its prefilter bind group.
        rhi::BindGroup* EnsurePrefilterSource(u32 slot);

        void DestroyResources();

        rhi::Device* m_device = nullptr;
        shaders::ShaderSystem* m_shaders = nullptr;

        // Captured -> prefiltered flip-blit (corrects RH-LookAt mirror). Per-face 2D SRVs + bind groups so
        // filtering stays within a face (no cross-face seams in smooth gradients like the sky).
        rhi::BindGroupLayout* m_blitLayout = nullptr;
        rhi::PipelineLayout* m_blitPipeLayout = nullptr;
        rhi::RenderPipeline* m_blitPipeline = nullptr;
        rhi::TextureView* m_capturedFaceView[kMaxProbes * 6] = {};
        rhi::BindGroup* m_blitFaceBG[kMaxProbes * 6] = {};

        // GGX roughness prefilter (prefiltered mip 0 -> mips 1..N-1).
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
        // startup submit cannot silently lose the one-shot bake (mirrors IBL's m_bakeWarmup).
        u32 m_captureWarmup = 20;
    };

} // namespace draconic::render
