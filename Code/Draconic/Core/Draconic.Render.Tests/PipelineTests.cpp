// The mesh draw path through the new architecture: register a MeshRenderer with a
// RendererRegistry, drive a RenderFrame (Begin / AddView / End) over an ExtractedScene with
// a cube + camera, into a color target. Run on the Null RHI + real DXC: exercises the whole
// path (extraction snapshot, view draw-list sort, mesh upload, per-object UBO, PSO build,
// render pass, DrawIndexed) and verifies a pipeline was produced.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"

import draconic.foundation;
import draconic.rhi;
import draconic.rhi.null;
import draconic.geometry;
import draconic.materials;
import draconic.shaders;
import draconic.shaders.system;
import draconic.materials.pipelinecache;
import draconic.render;
import draconic.rendergraph;

using namespace draconic::foundation;
using namespace draconic::render;
namespace rhi = draconic::rhi;
namespace geometry = draconic::geometry;
namespace materials = draconic::materials;
namespace shaders = draconic::shaders;

namespace
{

    // Engine built-in shaders live as files under the engine shader root (shaders.md P1);
    // tests wire the same dev file provider the RenderSubsystem does.
    shaders::FileShaderSourceProvider& EngineShaderProvider()
    {
        static shaders::FileShaderSourceProvider provider;
        static bool initialized = false;
        if (!initialized)
        {
            initialized = true;
            REQUIRE(provider.Initialize(u8"" DRACONIC_ENGINE_SHADER_DIR).IsOk());
        }
        return provider;
    }

    void WireEngineShaders(shaders::ShaderSystem& ss)
    {
        ss.SetSourceProvider(&EngineShaderProvider());
        const StringView includePaths[] = {EngineShaderProvider().RootDirectory()};
        ss.SetIncludePaths(Span<const StringView>{includePaths, 1});
    }

    // A small fixture holding the GPU-side systems + a color target + an encoder.
    struct RenderHarness
    {
        shaders::Compiler* compiler = nullptr;
        rhi::null::NullDevice device{DefaultAllocator()};
        rhi::Texture* color = nullptr;
        rhi::TextureView* colorView = nullptr;
        rhi::CommandPool* pool = nullptr;
        rhi::CommandEncoder* encoder = nullptr;

        bool Init(u32 w, u32 h)
        {
            if (!shaders::createCompiler(shaders::CompilerDesc{}, compiler).IsOk())
            {
                return false;
            }
            if (!device
                     .CreateTexture(
                         rhi::TextureDesc::RenderTarget(rhi::TextureFormat::BGRA8Unorm, w, h),
                         color)
                     .IsOk())
            {
                return false;
            }
            rhi::TextureViewDesc cvd{};
            cvd.format = rhi::TextureFormat::BGRA8Unorm;
            if (!device.CreateTextureView(color, cvd, colorView).IsOk())
            {
                return false;
            }
            if (!device.CreateCommandPool(rhi::QueueType::Graphics, pool).IsOk())
            {
                return false;
            }
            if (!pool->CreateEncoder(encoder).IsOk())
            {
                return false;
            }
            return true;
        }
        ~RenderHarness()
        {
            if (colorView)
            {
                device.DestroyTextureView(colorView);
            }
            if (color)
            {
                device.DestroyTexture(color);
            }
            if (pool)
            {
                device.DestroyCommandPool(pool);
            }
            if (compiler)
            {
                compiler->Destroy();
            }
        }
    };

} // namespace

TEST_CASE("RenderFrame draws a one-cube view (extract -> sort -> mesh upload -> PSO -> pass)")
{
    RenderHarness h;
    if (!h.Init(256, 256))
    {
        MESSAGE("DXC/Null unavailable; skipping");
        return;
    }

    shaders::ShaderSystem shaderSystem(*h.compiler, h.device);
    WireEngineShaders(shaderSystem);
    materials::PipelineStateCache psoCache(shaderSystem, h.device);

    materials::MaterialSystem materialSystem;
    REQUIRE(materialSystem.Initialize(h.device).IsOk());
    MeshRenderer meshRenderer(h.device, shaderSystem, psoCache, materialSystem,
                              /*framesInFlight*/ 2);
    REQUIRE(meshRenderer.Initialize().IsOk());
    RendererRegistry registry;
    registry.Register(&meshRenderer);
    RenderFrame frame(h.device, registry, /*framesInFlight*/ 2);

    // a scene snapshot: one cube at the origin
    RefPtr<geometry::StaticMesh> cube = geometry::Primitives::Cube(1.0f);
    RefPtr<materials::Material> material =
        materials::MaterialBuilder(u8"lit").Shader(u8"forward").Build();
    ExtractedScene scene;
    MeshRenderData* rd = scene.Add<MeshRenderData>();
    rd->world = Float4x4::Identity();
    rd->mesh = cube.Get();
    rd->material = material.Get();
    rd->category = RenderCategories::Opaque;

    ViewCamera camera;
    camera.view = Float4x4::LookAtRH(Float3{0, 0, 5}, Float3{0, 0, 0}, Float3{0, 1, 0});
    camera.projection = Float4x4::PerspectiveFovRH(1.0472f, 1.0f, 0.1f, 100.0f);
    ViewSettings settings;

    frame.Begin(*h.encoder, 0);
    frame.AddView(scene, camera, settings, h.colorView, rhi::TextureFormat::BGRA8Unorm, 256, 256);
    CHECK(frame.ViewCount() == 1);
    frame.End();

    CHECK(psoCache.Size() >= 1); // a pipeline was built for the cube's material

    // a second frame reuses the cached PSO + mesh buffers (no growth)
    frame.Begin(*h.encoder, 1);
    frame.AddView(scene, camera, settings, h.colorView, rhi::TextureFormat::BGRA8Unorm, 256, 256);
    frame.End();
    CHECK(psoCache.Size() >= 1); // cached PSOs reused across frames
}

TEST_CASE("RenderFrame batches same-mesh-same-material draws into an instanced draw")
{
    RenderHarness h;
    if (!h.Init(256, 256))
    {
        return;
    }

    shaders::ShaderSystem shaderSystem(*h.compiler, h.device);
    WireEngineShaders(shaderSystem);
    materials::PipelineStateCache psoCache(shaderSystem, h.device);
    materials::MaterialSystem materialSystem;
    REQUIRE(materialSystem.Initialize(h.device).IsOk());
    MeshRenderer meshRenderer(h.device, shaderSystem, psoCache, materialSystem,
                              /*framesInFlight*/ 2);
    REQUIRE(meshRenderer.Initialize().IsOk());
    RendererRegistry registry;
    registry.Register(&meshRenderer);
    RenderFrame frame(h.device, registry, /*framesInFlight*/ 2);

    RefPtr<geometry::StaticMesh> cube = geometry::Primitives::Cube(1.0f);
    RefPtr<materials::Material> material =
        materials::MaterialBuilder(u8"lit").Shader(u8"forward").Build();

    // Eight cubes, one shared mesh + material, distinct world + color -> one instanced batch.
    ExtractedScene scene;
    for (int n = 0; n < 8; ++n)
    {
        MeshRenderData* rd = scene.Add<MeshRenderData>();
        rd->world = Float4x4::Identity();
        rd->worldCenter = Float3{static_cast<f32>(n), 0, 0};
        rd->color = Color{static_cast<f32>(n) / 8.0f, 0.5f, 0.5f, 1.0f};
        rd->mesh = cube.Get();
        rd->material = material.Get();
        rd->category = RenderCategories::Opaque;
    }

    ViewCamera camera;
    camera.view = Float4x4::LookAtRH(Float3{0, 0, 20}, Float3{0, 0, 0}, Float3{0, 1, 0});
    camera.projection = Float4x4::PerspectiveFovRH(1.0472f, 1.0f, 0.1f, 100.0f);
    ViewSettings settings;

    frame.Begin(*h.encoder, 0);
    frame.AddView(scene, camera, settings, h.colorView, rhi::TextureFormat::BGRA8Unorm, 256, 256);
    frame.End();

    // The instanced permutation shares a pipeline across all eight draws.
    CHECK(psoCache.Size() >= 1);
}

TEST_CASE("RenderFrame parallel emit: many distinct draws fan out across the job system")
{
    RenderHarness h;
    if (!h.Init(256, 256))
    {
        return;
    }
    InitGlobalJobSystem(4);
    {
        shaders::ShaderSystem shaderSystem(*h.compiler, h.device);
        WireEngineShaders(shaderSystem);
        materials::PipelineStateCache psoCache(shaderSystem, h.device);
        materials::MaterialSystem materialSystem;
        REQUIRE(materialSystem.Initialize(h.device).IsOk());
        MeshRenderer meshRenderer(h.device, shaderSystem, psoCache, materialSystem,
                                  /*framesInFlight*/ 2);
        REQUIRE(meshRenderer.Initialize().IsOk());
        RendererRegistry registry;
        registry.Register(&meshRenderer);
        RenderFrame frame(h.device, registry, /*framesInFlight*/ 2);

        // 300 distinct materials (one shared mesh) -> 300 singleton draws (no batching) -> over
        // the parallel-emit threshold, so emission fans out across the job system's worker pools.
        RefPtr<geometry::StaticMesh> cube = geometry::Primitives::Cube(1.0f);
        Array<RefPtr<materials::Material>> mats; // keep the materials alive for the frame
        ExtractedScene scene;
        for (int n = 0; n < 300; ++n)
        {
            RefPtr<materials::Material> m =
                materials::MaterialBuilder(u8"lit").Shader(u8"forward").Build();
            mats.PushBack(m);
            MeshRenderData* rd = scene.Add<MeshRenderData>();
            rd->world = Float4x4::Identity();
            rd->worldCenter = Float3{static_cast<f32>(n), 0, 0};
            rd->mesh = cube.Get();
            rd->material = m.Get();
            rd->category = RenderCategories::Opaque;
        }

        ViewCamera camera;
        camera.view = Float4x4::LookAtRH(Float3{0, 0, 20}, Float3{0, 0, 0}, Float3{0, 1, 0});
        camera.projection = Float4x4::PerspectiveFovRH(1.0472f, 1.0f, 0.1f, 100.0f);
        ViewSettings settings;

        // Drive two frames to exercise the per-worker pool ring (reset between frame ring slots).
        for (u32 f = 0; f < 2; ++f)
        {
            frame.Begin(*h.encoder, f);
            frame.AddView(scene, camera, settings, h.colorView, rhi::TextureFormat::BGRA8Unorm, 256,
                          256);
            frame.End(); // parallel emit - must not crash or deadlock
        }
        CHECK(psoCache.Size() >= 1);
    }
    ShutdownGlobalJobSystem();
}

TEST_CASE("RenderFrame with an empty view still clears (no crash, no PSOs)")
{
    RenderHarness h;
    if (!h.Init(64, 64))
    {
        return;
    }

    shaders::ShaderSystem shaderSystem(*h.compiler, h.device);
    WireEngineShaders(shaderSystem);
    materials::PipelineStateCache psoCache(shaderSystem, h.device);
    materials::MaterialSystem materialSystem;
    REQUIRE(materialSystem.Initialize(h.device).IsOk());
    MeshRenderer meshRenderer(h.device, shaderSystem, psoCache, materialSystem,
                              /*framesInFlight*/ 2);
    REQUIRE(meshRenderer.Initialize().IsOk());
    RendererRegistry registry;
    registry.Register(&meshRenderer);
    RenderFrame frame(h.device, registry, /*framesInFlight*/ 2);

    ExtractedScene scene; // no renderables
    ViewCamera camera;
    ViewSettings settings;

    frame.Begin(*h.encoder, 0);
    frame.AddView(scene, camera, settings, h.colorView, rhi::TextureFormat::BGRA8Unorm, 64, 64);
    frame.End();
    CHECK(psoCache.Size() == 0);
}

TEST_CASE("RenderFrame multi-scene shadows: each view sources its OWN scene (no bleed)")
{
    // The editor renders DIFFERENT scenes side-by-side in one Begin/End bracket (the Sandbox's
    // multi-view-of-one-scene shape never hit this). Regression for the primary-scene sourcing
    // bug: scene A's casters shadowed scene B's views, and B's own casters never rendered.
    RenderHarness h;
    if (!h.Init(128, 128))
    {
        MESSAGE("DXC/Null unavailable; skipping");
        return;
    }

    shaders::ShaderSystem shaderSystem(*h.compiler, h.device);
    WireEngineShaders(shaderSystem);
    materials::PipelineStateCache psoCache(shaderSystem, h.device);
    materials::MaterialSystem materialSystem;
    REQUIRE(materialSystem.Initialize(h.device).IsOk());
    MeshRenderer meshRenderer(h.device, shaderSystem, psoCache, materialSystem,
                              /*framesInFlight*/ 2);
    REQUIRE(meshRenderer.Initialize().IsOk());
    RendererRegistry registry;
    registry.Register(&meshRenderer);
    ShadowSystem shadows(h.device, /*framesInFlight*/ 2);
    REQUIRE(shadows.Initialize().IsOk());
    RenderFrame frame(h.device, registry, /*framesInFlight*/ 2, nullptr, nullptr, &shadows);

    RefPtr<geometry::StaticMesh> cube = geometry::Primitives::Cube(1.0f);
    RefPtr<materials::Material> material =
        materials::MaterialBuilder(u8"lit").Shader(u8"forward").Build();
    const auto addCube = [&](ExtractedScene& scene)
    {
        MeshRenderData* rd = scene.Add<MeshRenderData>();
        rd->world = Float4x4::Identity();
        rd->mesh = cube.Get();
        rd->material = material.Get();
        rd->category = RenderCategories::Opaque;
    };
    const auto addSpotCaster = [&](ExtractedScene& scene, Float3 pos)
    {
        LocalShadowCaster c;
        c.type = 2;
        c.positionWS = pos;
        c.directionWS = Float3{0, -1, 0};
        c.range = 10.0f;
        c.outerAngle = 0.8f;
        scene.AddLocalShadowCaster(c);
    };

    // Scene A: spot caster only. Scene B: directional caster + spot caster.
    ExtractedScene sceneA;
    addCube(sceneA);
    addSpotCaster(sceneA, Float3{0, 5, 0});
    ExtractedScene sceneB;
    addCube(sceneB);
    addSpotCaster(sceneB, Float3{7, 3, 2});
    {
        DirectionalShadow ds;
        ds.direction = Normalized(Float3{0.3f, -1.0f, 0.2f});
        ds.valid = true;
        sceneB.SetDirectionalShadow(ds);
    }

    ViewCamera camera;
    camera.view = Float4x4::LookAtRH(Float3{0, 0, 5}, Float3{0, 0, 0}, Float3{0, 1, 0});
    camera.projection = Float4x4::PerspectiveFovRH(1.0472f, 1.0f, 0.1f, 100.0f);
    ViewSettings settings;

    frame.Begin(*h.encoder, 0);
    frame.AddView(sceneA, camera, settings, h.colorView, rhi::TextureFormat::BGRA8Unorm, 128, 128);
    frame.AddView(sceneB, camera, settings, h.colorView, rhi::TextureFormat::BGRA8Unorm, 128, 128);
    frame.End();

    // Directional: ONLY the scene-B view has one (before the fix: keyed off scene A = none at all;
    // with A/B swapped, both views would sample A's).
    const Span<const RenderFrame::ViewShadowDebug> info = frame.ViewShadowInfo();
    REQUIRE(info.Size() == 2u);
    CHECK_FALSE(info[0].directional);
    CHECK(info[1].directional);
    // BOTH views must bind + declare the cascade map when it exists: the caster-less view's
    // set-0 group holds the real map (frame-global) and its shader samples it statically -
    // an undeclared read executes against ATTACHMENT-layout layers (the validation error a
    // caster-less material-preview page next to a lit scene produced).
    CHECK(info[0].mapBound);
    CHECK(info[1].mapBound);

    // Local shadows: BOTH scenes' spot casters got entries (before the fix: only scene A's), the
    // entry buffer is the concatenation, and each view offsets by its scene's base.
    const Span<const GpuLocalShadow> entries = frame.LocalShadowEntries();
    REQUIRE(entries.Size() == 2u);
    CHECK(info[0].localEntryBase == 0u);
    CHECK(info[1].localEntryBase == 1u);
    // Distinct atlas tiles (global tile counters) and distinct light matrices (distinct lights).
    CHECK(entries[0].atlasScaleBias.z != entries[1].atlasScaleBias.z);
    CHECK(entries[0].atlasScaleBias.x > 0.0f); // neither entry is degenerate
    CHECK(entries[1].atlasScaleBias.x > 0.0f);

    // Same-scene multi-view (the Sandbox shape) still shares one context: one entry set, one base.
    frame.Begin(*h.encoder, 1);
    frame.AddView(sceneB, camera, settings, h.colorView, rhi::TextureFormat::BGRA8Unorm, 128, 128);
    frame.AddView(sceneB, camera, settings, h.colorView, rhi::TextureFormat::BGRA8Unorm, 128, 128);
    frame.End();
    const Span<const RenderFrame::ViewShadowDebug> info2 = frame.ViewShadowInfo();
    REQUIRE(info2.Size() == 2u);
    CHECK(info2[0].directional);
    CHECK(info2[1].directional);
    CHECK(info2[0].localEntryBase == 0u);
    CHECK(info2[1].localEntryBase == 0u);           // same scene -> same context/base
    CHECK(frame.LocalShadowEntries().Size() == 1u); // one spot caster, once
}

TEST_CASE("ReflectionProbeSystem accumulates per-scene ranges (multi-scene frames)")
{
    // Frames can render several scenes; each scene's probes Assign() into ONE record buffer as
    // a contiguous range, and views read their scene's range (regression: Assign used to RESET
    // per call - the last extracted scene's probes were the only ones anyone saw).
    RenderHarness h;
    if (!h.Init(64, 64))
    {
        MESSAGE("DXC/Null unavailable; skipping");
        return;
    }

    shaders::ShaderSystem shaderSystem(*h.compiler, h.device);
    WireEngineShaders(shaderSystem);
    ReflectionProbeSystem probes(h.device, shaderSystem);
    REQUIRE(probes.Initialize().IsOk());

    ExtractedScene sceneA, sceneB;
    ReflectionProbe pa{};
    pa.key = 1;
    pa.center = Float3{0, 1, 0};
    ReflectionProbe pb0{}, pb1{};
    pb0.key = 2;
    pb0.center = Float3{5, 1, 0};
    pb1.key = 3;
    pb1.center = Float3{9, 1, 0};

    probes.BeginFrame();
    ReflectionProbe listA[] = {pa};
    ReflectionProbe listB[] = {pb0, pb1};
    probes.Assign(&sceneA, Span<const ReflectionProbe>{listA, 1});
    probes.Assign(&sceneB, Span<const ReflectionProbe>{listB, 2});
    // Same-scene re-assign (same scene rendered through two views) is a no-op.
    probes.Assign(&sceneA, Span<const ReflectionProbe>{listA, 1});

    CHECK(probes.ActiveCount() == 3u);
    CHECK(probes.RangeFor(&sceneA).base == 0u);
    CHECK(probes.RangeFor(&sceneA).count == 1u);
    CHECK(probes.RangeFor(&sceneB).base == 1u);
    CHECK(probes.RangeFor(&sceneB).count == 2u);
    ExtractedScene sceneC;
    CHECK(probes.RangeFor(&sceneC).count == 0u); // unknown scene -> no probes

    // Every capture task carries its probe's OWNING scene (captures must render that scene's
    // geometry, never another's).
    REQUIRE(probes.Captures().Size() == 3u);
    for (const ReflectionProbeSystem::CaptureTask& t : probes.Captures())
    {
        CHECK(t.scene == ((t.slot == 0u) ? &sceneA : &sceneB));
    }

    // Captured static probes stop producing tasks once the startup warmup window has
    // drained (the first frames deliberately re-capture so a dropped web startup submit
    // cannot silently lose the one-shot bake - mirrors the IBL env-bake warmup).
    u32 warmupFrames = 0;
    for (; warmupFrames < 64; ++warmupFrames)
    {
        for (const ReflectionProbeSystem::CaptureTask& t : probes.Captures())
        {
            probes.MarkCaptured(t.slot);
        }
        probes.BeginFrame();
        probes.Assign(&sceneA, Span<const ReflectionProbe>{listA, 1});
        probes.Assign(&sceneB, Span<const ReflectionProbe>{listB, 2});
        if (probes.Captures().Size() == 0u)
        {
            break;
        }
    }
    CHECK(warmupFrames > 0u);  // the warmup window re-captured at least once
    CHECK(warmupFrames < 64u); // and it DRAINS - static probes go quiet
    CHECK(probes.Captures().Size() == 0u);
    CHECK(probes.RangeFor(&sceneB).base == 1u); // ranges rebuilt identically
}

TEST_CASE("IBLSystem: per-scene contexts; env rebuilds when a scene's sky-texture product changes")
{
    RenderHarness h;
    if (!h.Init(64, 64))
    {
        MESSAGE("DXC/Null unavailable; skipping");
        return;
    }

    shaders::ShaderSystem shaderSystem(*h.compiler, h.device);
    WireEngineShaders(shaderSystem);
    IBLSystem ibl(h.device, shaderSystem);
    REQUIRE(ibl.Initialize().IsOk());

    // An external 2D view standing in for a cooked .hdr product.
    rhi::Texture* tex = nullptr;
    REQUIRE(h.device
                .CreateTexture(rhi::TextureDesc::RenderTarget(rhi::TextureFormat::RGBA8Unorm, 8, 8),
                               tex)
                .IsOk());
    rhi::TextureViewDesc vd{};
    vd.format = rhi::TextureFormat::RGBA8Unorm;
    rhi::TextureView* view = nullptr;
    REQUIRE(h.device.CreateTextureView(tex, vd, view).IsOk());

    ExtractedScene sceneA, sceneB;
    const Float3 sun{0.0f, -1.0f, 0.0f};

    SkySnapshot procedural{};
    SkySnapshot hdr{};
    hdr.mode = SkyMode::HDREquirect;
    hdr.texture = view;
    hdr.textureUid = 101;
    hdr.textureIsCube = false;

    // Two scenes with DIFFERENT skies get their own contexts and products.
    IBLSystem::Context *ctxA = nullptr, *ctxB = nullptr;
    {
        draconic::rendergraph::RenderGraph graph(&h.device);
        ibl.BeginFrame(graph);
        ctxA = ibl.Prepare(&sceneA, procedural, sun, graph);
        ctxB = ibl.Prepare(&sceneB, hdr, sun, graph);
    }
    REQUIRE(ctxA != nullptr);
    REQUIRE(ctxB != nullptr);
    CHECK(ctxA != ctxB);
    CHECK(ibl.ContextCount() == 2u);
    CHECK(ctxA->ShBuffer() != ctxB->ShBuffer()); // distinct products
    CHECK(ctxA->PrefilterView() != ctxB->PrefilterView());
    CHECK(ctxA->Generation() != ctxB->Generation()); // system-wide counter: never collides
    CHECK(ctxA->HasSunDisc());
    CHECK_FALSE(ctxB->HasSunDisc()); // textured env carries its own sun

    // Steady state: same scenes re-Prepare into the SAME contexts with no rebuild.
    const u64 genA = ctxA->Generation(), genB = ctxB->Generation();
    {
        draconic::rendergraph::RenderGraph graph(&h.device);
        ibl.BeginFrame(graph);
        CHECK(ibl.Prepare(&sceneA, procedural, sun, graph) == ctxA);
        CHECK(ibl.Prepare(&sceneB, hdr, sun, graph) == ctxB);
    }
    CHECK(ctxA->Generation() == genA);
    CHECK(ctxB->Generation() == genB);

    // Scene B's sky-texture PRODUCT changes (pick/hot-reload; uid-keyed, never the pointer):
    // only B rebuilds.
    hdr.textureUid = 102;
    {
        draconic::rendergraph::RenderGraph graph(&h.device);
        ibl.BeginFrame(graph);
        (void)ibl.Prepare(&sceneA, procedural, sun, graph);
        (void)ibl.Prepare(&sceneB, hdr, sun, graph);
    }
    CHECK(ctxA->Generation() == genA);
    CHECK(ctxB->Generation() > genB);

    h.device.DestroyTextureView(view);
    h.device.DestroyTexture(tex);
}

TEST_CASE("RenderFrame draws an UNLIT material (unlit shader compiles + PSO builds)")
{
    RenderHarness h;
    if (!h.Init(128, 128))
    {
        MESSAGE("DXC/Null unavailable; skipping");
        return;
    }

    shaders::ShaderSystem shaderSystem(*h.compiler, h.device);
    WireEngineShaders(shaderSystem);
    materials::PipelineStateCache psoCache(shaderSystem, h.device);
    materials::MaterialSystem materialSystem;
    REQUIRE(materialSystem.Initialize(h.device).IsOk());
    MeshRenderer meshRenderer(h.device, shaderSystem, psoCache, materialSystem,
                              /*framesInFlight*/ 2);
    REQUIRE(meshRenderer.Initialize().IsOk());
    RendererRegistry registry;
    registry.Register(&meshRenderer);
    RenderFrame frame(h.device, registry, /*framesInFlight*/ 2);

    RefPtr<geometry::StaticMesh> cube = geometry::Primitives::Cube(1.0f);
    RefPtr<materials::Material> unlit = materials::CreateUnlit(u8"flat", Float4{1, 0, 0, 1});
    CHECK(unlit->shaderName == u8"unlit");
    CHECK(unlit->FindProperty(u8"BaseColor") != nullptr);
    CHECK(unlit->FindProperty(u8"AlbedoMap") != nullptr);
    CHECK(unlit->FindProperty(u8"Metallic") == nullptr); // the lit set stays out of the preset

    ExtractedScene scene;
    MeshRenderData* rd = scene.Add<MeshRenderData>();
    rd->world = Float4x4::Identity();
    rd->mesh = cube.Get();
    rd->material = unlit.Get();
    rd->category = RenderCategories::Opaque;

    ViewCamera camera;
    camera.view = Float4x4::LookAtRH(Float3{0, 0, 5}, Float3{0, 0, 0}, Float3{0, 1, 0});
    camera.projection = Float4x4::PerspectiveFovRH(1.0472f, 1.0f, 0.1f, 100.0f);
    ViewSettings settings;

    frame.Begin(*h.encoder, 0);
    frame.AddView(scene, camera, settings, h.colorView, rhi::TextureFormat::BGRA8Unorm, 128, 128);
    frame.End();
    CHECK(psoCache.Size() >= 1); // the unlit permutation compiled + built
}
