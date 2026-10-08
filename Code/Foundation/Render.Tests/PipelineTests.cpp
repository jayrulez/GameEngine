// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The mesh draw path through the new architecture: register a MeshRenderer with a
// RendererRegistry, drive a RenderFrame (Begin / AddView / End) over an ExtractedScene with
// a cube + camera, into a color target. Run on the Null RHI + real DXC: exercises the whole
// path (extraction snapshot, view draw-list sort, mesh upload, per-object UBO, PSO build,
// render pass, DrawIndexed) and verifies a pipeline was produced.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.vfs; // the engine data root (FindDataRoot / DataPath)
import foundation.rhi;
import foundation.rhi.null;
import foundation.geometry;
import foundation.materials;
import foundation.shaders;
import foundation.shaders.system;
import foundation.materials.pipelinecache;
import foundation.render;
import foundation.rendergraph;

using namespace foundation::core;
using namespace foundation::render;
namespace rhi = foundation::rhi;
namespace geometry = foundation::geometry;
namespace materials = foundation::materials;
namespace shaders = foundation::shaders;

namespace
{

    // Engine built-in shaders live as files under the data root's Shaders/;
    // tests wire the same dev file provider the RenderSubsystem does.
    shaders::FileShaderSourceProvider& EngineShaderProvider()
    {
        static shaders::FileShaderSourceProvider provider{DefaultAllocator()};
        static bool initialized = false;
        if (!initialized)
        {
            initialized = true;
            // The engine corpus, found the way every executable finds it: the data root's
            // Shaders/ through a mount over it (includes resolve through the same mount).
            static foundation::vfs::NativeFileSystem dataFs(foundation::vfs::FindDataRoot(),
                                                            DefaultAllocator());
            REQUIRE(provider.Initialize(dataFs, shaders::kShaderFolder).IsOk());
        }
        return provider;
    }

    void WireEngineShaders(shaders::ShaderSystem& ss)
    {
        ss.SetSourceProvider(&EngineShaderProvider());
        ss.SetIncludeResolver(&EngineShaderProvider());
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
    RenderFrame frame(DefaultAllocator(), h.device, registry, /*framesInFlight*/ 2);

    // a scene snapshot: one cube at the origin
    RefPtr<geometry::StaticMesh> cube = geometry::Primitives::Cube(DefaultAllocator(), 1.0f);
    RefPtr<materials::Material> material =
        materials::MaterialBuilder(u8"lit").Shader(u8"forward").Build();
    ExtractedScene scene{DefaultAllocator()};
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

TEST_CASE("RenderFrame: the bone pool starts small and grows the frame that needs more")
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
    RenderFrame frame(DefaultAllocator(), h.device, registry, /*framesInFlight*/ 2);

    // A minimal skinned mesh: one triangle, each vertex bound to one joint.
    RefPtr<geometry::SkinnedMesh> skinned = MakeRef<geometry::SkinnedMesh>(DefaultAllocator());
    for (u32 i = 0; i < 3; ++i)
    {
        skinned->vertices.PushBack(geometry::StaticMeshVertex{
            Float3{static_cast<f32>(i), 0, 0}, Float3{0, 1, 0}, Float2{0, 0}, 0xFFFFFFFFu,
            Float3{1, 0, 0}});
        geometry::VertexSkinning s{};
        s.joints[0] = static_cast<u16>(i);
        s.weights = Float4{1, 0, 0, 0};
        skinned->skinning.PushBack(s);
    }
    skinned->indices.Resize(3);
    skinned->indices.AddTriangle(0, 1, 2);
    RefPtr<materials::Material> material =
        materials::MaterialBuilder(u8"lit").Shader(u8"forward").Build();

    // Distinct casters dedupe by their bone-matrix POINTER, so each gets its own palette.
    constexpr u32 kBones = 64;
    constexpr u32 kCasters = 40; // 40 x 64 x 2 (current + previous slab) = 5120 > 4096
    Array<Array<Float4x4>> palettes;
    palettes.Resize(kCasters);
    ExtractedScene scene{DefaultAllocator()};
    for (u32 c = 0; c < kCasters; ++c)
    {
        palettes[c].Resize(kBones, Float4x4::Identity());
        MeshRenderData* rd = scene.Add<MeshRenderData>();
        rd->world = Float4x4::Identity();
        rd->mesh = skinned.Get();
        rd->material = material.Get();
        rd->category = RenderCategories::Opaque;
        rd->boneMatrices = palettes[c].Data();
        rd->boneCount = kBones;
    }
    ViewCamera camera;
    camera.view = Float4x4::LookAtRH(Float3{0, 0, 5}, Float3{0, 0, 0}, Float3{0, 1, 0});
    camera.projection = Float4x4::PerspectiveFovRH(1.0472f, 1.0f, 0.1f, 100.0f);
    ViewSettings settings;

    // Nothing is reserved before the first frame (PrepareFrame sizes the rings).
    CHECK(meshRenderer.BonePoolSlotsPerFrame() == 0u);

    // The first frame with 5120 matrices grows the pool to the next power of two - in the
    // same frame, so nothing renders unskinned - and the next frame keeps it.
    frame.Begin(*h.encoder, 0);
    frame.AddView(scene, camera, settings, h.colorView, rhi::TextureFormat::BGRA8Unorm, 256, 256);
    frame.End();
    CHECK(meshRenderer.BonePoolSlotsPerFrame() == 8192u);
    frame.Begin(*h.encoder, 1);
    frame.AddView(scene, camera, settings, h.colorView, rhi::TextureFormat::BGRA8Unorm, 256, 256);
    frame.End();
    CHECK(meshRenderer.BonePoolSlotsPerFrame() == 8192u);

    // A scene that fits never grows it.
    MeshRenderer small(h.device, shaderSystem, psoCache, materialSystem, 2);
    REQUIRE(small.Initialize().IsOk());
    RendererRegistry smallRegistry;
    smallRegistry.Register(&small);
    RenderFrame smallFrame(DefaultAllocator(), h.device, smallRegistry, 2);
    ExtractedScene one{DefaultAllocator()};
    MeshRenderData* rd = one.Add<MeshRenderData>();
    rd->world = Float4x4::Identity();
    rd->mesh = skinned.Get();
    rd->material = material.Get();
    rd->category = RenderCategories::Opaque;
    rd->boneMatrices = palettes[0].Data();
    rd->boneCount = kBones;
    smallFrame.Begin(*h.encoder, 0);
    smallFrame.AddView(one, camera, settings, h.colorView, rhi::TextureFormat::BGRA8Unorm, 256, 256);
    smallFrame.End();
    CHECK(small.BonePoolSlotsPerFrame() == MeshRenderer::InitialBonePoolSlots());
}

namespace
{
    // An external renderer's Opaque data: base fields + a payload that is NOT a MeshRenderData.
    // Every byte past the base is 0xFF, so a blind MeshRenderData downcast would read a non-null
    // boneMatrices with a huge boneCount and file it as an animated caster.
    struct JunkRenderData : RenderData
    {
        JunkRenderData() { MemSet(junk, 0xFF, sizeof(junk)); }
        u8 junk[512];
    };

    // A minimal external renderer that claims Opaque and draws nothing.
    class InertOpaqueRenderer final : public Renderer
    {
    public:
        [[nodiscard]] Span<const RenderCategory> SupportedCategories() const override
        {
            static const RenderCategory kCats[] = {RenderCategories::Opaque};
            return Span<const RenderCategory>{kCats, 1};
        }
        void Resolve(const RenderRecordContext&, Span<const DrawItem>, Array<ResolvedDraw>&) override
        {
        }
        void ResolveDepthOnly(const RenderRecordContext&, Span<const DrawItem>,
                              Array<ResolvedDraw>&) override
        {
        }
    };
}

TEST_CASE("RenderData::kind says what an item is; the caster list never downcasts by renderer id")
{
    // The stamp: the base is Generic, MeshRenderData and everything derived from it is Mesh.
    CHECK(RenderData{}.kind == RenderDataKind::Generic);
    CHECK(MeshRenderData{}.kind == RenderDataKind::Mesh);
    CHECK(MultiMeshRenderData{}.kind == RenderDataKind::Mesh);
    CHECK(JunkRenderData{}.kind == RenderDataKind::Generic);

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
    MeshRenderer meshRenderer(h.device, shaderSystem, psoCache, materialSystem, 2);
    REQUIRE(meshRenderer.Initialize().IsOk());
    // The external renderer registers FIRST, so it - not the mesh renderer - holds id 0. That is
    // the terrain probe's shape (it registers terrain alone) and any embedding that adds its own
    // renderer before the built-in one.
    InertOpaqueRenderer external;
    RendererRegistry registry;
    registry.Register(&external);
    registry.Register(&meshRenderer);
    REQUIRE(external.RendererId() == 0u);
    REQUIRE(meshRenderer.RendererId() == 1u);
    ShadowSystem shadows(h.device, 2);
    REQUIRE(shadows.Initialize().IsOk());
    RenderFrame frame(DefaultAllocator(), h.device, registry, 2, nullptr, nullptr, &shadows);

    // Two junk casters from the external renderer (id 0) + one skinned mesh caster (id 1).
    RefPtr<geometry::SkinnedMesh> skinned = MakeRef<geometry::SkinnedMesh>(DefaultAllocator());
    for (u32 i = 0; i < 3; ++i)
    {
        skinned->vertices.PushBack(geometry::StaticMeshVertex{
            Float3{static_cast<f32>(i), 0, 0}, Float3{0, 1, 0}, Float2{0, 0}, 0xFFFFFFFFu,
            Float3{1, 0, 0}});
        geometry::VertexSkinning vs{};
        vs.joints[0] = static_cast<u16>(i);
        vs.weights = Float4{1, 0, 0, 0};
        skinned->skinning.PushBack(vs);
    }
    skinned->indices.Resize(3);
    skinned->indices.AddTriangle(0, 1, 2);
    RefPtr<materials::Material> material =
        materials::MaterialBuilder(u8"lit").Shader(u8"forward").Build();
    Array<Float4x4> palette;
    palette.Resize(8, Float4x4::Identity());

    ExtractedScene scene{DefaultAllocator()};
    for (int i = 0; i < 2; ++i)
    {
        JunkRenderData* junk = scene.Add<JunkRenderData>();
        junk->category = RenderCategories::Opaque;
        junk->rendererId = external.RendererId();
        junk->worldCenter = Float3{static_cast<f32>(i), 0, 0};
        junk->worldRadius = 1.0f;
    }
    MeshRenderData* rd = scene.Add<MeshRenderData>();
    rd->world = Float4x4::Identity();
    rd->mesh = skinned.Get();
    rd->material = material.Get();
    rd->category = RenderCategories::Opaque;
    rd->rendererId = meshRenderer.RendererId();
    rd->boneMatrices = palette.Data();
    rd->boneCount = 8;
    rd->worldRadius = 1.0f;
    DirectionalShadow ds;
    ds.direction = Normalized(Float3{0.3f, -1.0f, 0.2f});
    ds.valid = true;
    scene.SetDirectionalShadow(ds);

    ViewCamera camera;
    camera.view = Float4x4::LookAtRH(Float3{0, 0, 5}, Float3{0, 0, 0}, Float3{0, 1, 0});
    camera.projection = Float4x4::PerspectiveFovRH(1.0472f, 1.0f, 0.1f, 100.0f);
    ViewSettings settings;
    frame.Begin(*h.encoder, 0);
    frame.AddView(scene, camera, settings, h.colorView, rhi::TextureFormat::BGRA8Unorm, 128, 128);
    frame.End();

    // All three are casters (base fields only); exactly the skinned MESH is an animated caster.
    // The id gate read 2 animated casters here (the junk bytes) and missed the real one.
    CHECK(frame.ShadowCasterCount(&scene) == 3u);
    CHECK(frame.MovingShadowCasterCount(&scene) == 1u);
}

TEST_CASE("RenderFrame: a caster that moves, appears or goes counts as moving for cached shadows")
{
    RenderHarness h;
    if (!h.Init(128, 128))
    {
        MESSAGE("DXC/Null unavailable; skipping");
        return;
    }
    InertOpaqueRenderer external;
    RendererRegistry registry;
    registry.Register(&external);
    ShadowSystem shadows(h.device, 2);
    REQUIRE(shadows.Initialize().IsOk());
    RenderFrame frame(DefaultAllocator(), h.device, registry, 2, nullptr, nullptr, &shadows);

    // A door and a wall (plain casters, no bones) by a torch whose shadow is cached.
    ExtractedScene scene{DefaultAllocator()};
    JunkRenderData* door = scene.Add<JunkRenderData>();
    JunkRenderData* wall = scene.Add<JunkRenderData>();
    u64 id = 7;
    JunkRenderData* both[] = {door, wall};
    for (JunkRenderData* d : both)
    {
        d->category = RenderCategories::Opaque;
        d->rendererId = external.RendererId();
        d->worldRadius = 1.0f;
        d->entityId = id++;
    }
    wall->worldCenter = Float3{3.0f, 1.5f, 0.0f};
    LocalShadowCaster torch;
    torch.type = 1;
    torch.positionWS = Float3{1.0f, 1.5f, 1.0f};
    torch.range = 6.0f;
    torch.isStatic = true;
    scene.AddLocalShadowCaster(torch);

    ViewCamera camera;
    camera.view = Float4x4::LookAtRH(Float3{0, 0, 5}, Float3{0, 0, 0}, Float3{0, 1, 0});
    camera.projection = Float4x4::PerspectiveFovRH(1.0472f, 1.0f, 0.1f, 100.0f);
    const auto render = [&]()
    {
        ViewSettings settings;
        frame.Begin(*h.encoder, 0);
        frame.AddView(scene, camera, settings, h.colorView, rhi::TextureFormat::BGRA8Unorm, 128, 128);
        frame.End();
        return frame.MovingShadowCasterCount(&scene);
    };

    CHECK(render() == 2u); // both new to the cache
    CHECK(render() == 0u); // still
    door->worldCenter = Float3{0.5f, 1.0f, 0.5f}; // the door swings
    CHECK(render() == 1u);
    CHECK(render() == 0u); // and stops
    door->castShadows = false; // gone from the casters
    CHECK(render() == 1u);
    CHECK(render() == 0u);
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
    RenderFrame frame(DefaultAllocator(), h.device, registry, /*framesInFlight*/ 2);

    RefPtr<geometry::StaticMesh> cube = geometry::Primitives::Cube(DefaultAllocator(), 1.0f);
    RefPtr<materials::Material> material =
        materials::MaterialBuilder(u8"lit").Shader(u8"forward").Build();

    // Eight cubes, one shared mesh + material, distinct world + color -> one instanced batch.
    ExtractedScene scene{DefaultAllocator()};
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
        RenderFrame frame(DefaultAllocator(), h.device, registry, /*framesInFlight*/ 2);

        // 300 distinct materials (one shared mesh) -> 300 singleton draws (no batching) -> over
        // the parallel-emit threshold, so emission fans out across the job system's worker pools.
        RefPtr<geometry::StaticMesh> cube = geometry::Primitives::Cube(DefaultAllocator(), 1.0f);
        Array<RefPtr<materials::Material>> mats; // keep the materials alive for the frame
        ExtractedScene scene{DefaultAllocator()};
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
    RenderFrame frame(DefaultAllocator(), h.device, registry, /*framesInFlight*/ 2);

    ExtractedScene scene{DefaultAllocator()}; // no renderables
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
    RenderFrame frame(DefaultAllocator(), h.device, registry, /*framesInFlight*/ 2, nullptr, nullptr, &shadows);

    RefPtr<geometry::StaticMesh> cube = geometry::Primitives::Cube(DefaultAllocator(), 1.0f);
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
    ExtractedScene sceneA{DefaultAllocator()};
    addCube(sceneA);
    addSpotCaster(sceneA, Float3{0, 5, 0});
    ExtractedScene sceneB{DefaultAllocator()};
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
    // a contiguous range, and views read their scene's range. Assign must not RESET
    // per call, or only the last extracted scene's probes would be visible.
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

    ExtractedScene sceneA{DefaultAllocator()}, sceneB{DefaultAllocator()};
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
    ExtractedScene sceneC{DefaultAllocator()};
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

    ExtractedScene sceneA{DefaultAllocator()}, sceneB{DefaultAllocator()};
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
        foundation::rendergraph::RenderGraph graph(DefaultAllocator(), &h.device);
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
        foundation::rendergraph::RenderGraph graph(DefaultAllocator(), &h.device);
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
        foundation::rendergraph::RenderGraph graph(DefaultAllocator(), &h.device);
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
    RenderFrame frame(DefaultAllocator(), h.device, registry, /*framesInFlight*/ 2);

    RefPtr<geometry::StaticMesh> cube = geometry::Primitives::Cube(DefaultAllocator(), 1.0f);
    RefPtr<materials::Material> unlit = materials::CreateUnlit(u8"flat", Float4{1, 0, 0, 1});
    CHECK(unlit->shaderName == u8"unlit");
    CHECK(unlit->FindProperty(u8"BaseColor") != nullptr);
    CHECK(unlit->FindProperty(u8"AlbedoMap") != nullptr);
    CHECK(unlit->FindProperty(u8"Metallic") == nullptr); // the lit set stays out of the preset

    ExtractedScene scene{DefaultAllocator()};
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

namespace
{
    // Captures the RenderRecordContext fields per pass kind - the regression net for the
    // prepass-LOD bug: the depth prepass MUST carry the same camera view matrix as the color
    // pass, or per-view LOD selection (terrain chunk coverage, mesh LOD chains) picks DIFFERENT
    // levels in the two passes and their depths z-fight (far-chunk row banding).
    class CtxCaptureRenderer final : public Renderer
    {
    public:
        [[nodiscard]] Span<const RenderCategory> SupportedCategories() const override
        {
            static const RenderCategory kCats[] = {RenderCategories::Opaque};
            return Span<const RenderCategory>{kCats, 1};
        }
        void Resolve(const RenderRecordContext& ctx, Span<const DrawItem>,
                     Array<ResolvedDraw>&) override
        {
            colorViewMatrix = ctx.viewMatrix;
            colorSeen = true;
        }
        void ResolveDepthOnly(const RenderRecordContext& ctx, Span<const DrawItem>,
                              Array<ResolvedDraw>&) override
        {
            if (ctx.depthPrepass)
            {
                prepassViewMatrix = ctx.viewMatrix;
                prepassSeen = true;
            }
        }
        Float4x4 colorViewMatrix = Float4x4::Identity();
        Float4x4 prepassViewMatrix = Float4x4::Identity();
        bool colorSeen = false;
        bool prepassSeen = false;
    };

    [[nodiscard]] bool SameMatrix(const Float4x4& a, const Float4x4& b)
    {
        for (i32 r = 0; r < 4; ++r)
        {
            for (i32 c = 0; c < 4; ++c)
            {
                if (a.m[r][c] != b.m[r][c])
                {
                    return false;
                }
            }
        }
        return true;
    }
}

TEST_CASE("depth prepass context carries the camera view matrix (per-view LOD parity)")
{
    RenderHarness h;
    if (!h.Init(256, 256))
    {
        return;
    }
    CtxCaptureRenderer capture;
    RendererRegistry registry;
    registry.Register(&capture);
    RenderFrame frame(DefaultAllocator(), h.device, registry, /*framesInFlight*/ 2);

    ExtractedScene scene{DefaultAllocator()};
    MeshRenderData* rd = scene.Add<MeshRenderData>();
    rd->world = Float4x4::Identity();
    rd->category = RenderCategories::Opaque;
    rd->rendererId = capture.RendererId();

    ViewCamera camera;
    camera.view = Float4x4::LookAtRH(Float3{10, 20, 30}, Float3{0, 0, 0}, Float3{0, 1, 0});
    camera.projection = Float4x4::PerspectiveFovRH(1.0f, 1.0f, 0.1f, 1000.0f);
    ViewSettings settings;

    frame.Begin(*h.encoder, 0);
    frame.AddView(scene, camera, settings, h.colorView, rhi::TextureFormat::BGRA8Unorm, 256, 256);
    frame.End();

    REQUIRE(capture.colorSeen);
    REQUIRE(capture.prepassSeen);
    // BOTH passes saw the REAL camera view (not identity), and the SAME one - the invariant that
    // keeps per-view LOD selection identical between the prepass and the color pass.
    CHECK_FALSE(SameMatrix(capture.prepassViewMatrix, Float4x4::Identity()));
    CHECK(SameMatrix(capture.prepassViewMatrix, camera.view));
    CHECK(SameMatrix(capture.colorViewMatrix, camera.view));
}

TEST_CASE("debug view: a named graph texture appends the blit pass; the inventory lists it")
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

    DebugBlitPass debugBlit(h.device, shaderSystem, /*framesInFlight*/ 2);
    REQUIRE(debugBlit.Initialize().IsOk());
    RenderFrame frame(DefaultAllocator(), h.device, registry, /*framesInFlight*/ 2,
                      /*clusters*/ nullptr, /*tonemap*/ nullptr, /*shadows*/ nullptr,
                      /*ibl*/ nullptr, /*sky*/ nullptr, /*bloom*/ nullptr, /*taa*/ nullptr,
                      /*ao*/ nullptr, /*fxaa*/ nullptr, /*exposure*/ nullptr, &debugBlit);

    RefPtr<geometry::StaticMesh> cube = geometry::Primitives::Cube(DefaultAllocator(), 1.0f);
    RefPtr<materials::Material> material =
        materials::MaterialBuilder(u8"lit").Shader(u8"forward").Build();
    ExtractedScene scene{DefaultAllocator()};
    MeshRenderData* rd = scene.Add<MeshRenderData>();
    rd->world = Float4x4::Identity();
    rd->mesh = cube.Get();
    rd->material = material.Get();
    rd->category = RenderCategories::Opaque;

    ViewCamera camera;
    camera.view = Float4x4::LookAtRH(Float3{0, 0, 5}, Float3{0, 0, 0}, Float3{0, 1, 0});
    camera.projection = Float4x4::PerspectiveFovRH(1.0472f, 1.0f, 0.1f, 100.0f);

    const auto hasPass = [&](StringView name)
    {
        for (const foundation::rendergraph::RenderGraphPass* pass : frame.Graph().Passes())
        {
            if (pass != nullptr && pass->name == name)
            {
                return true;
            }
        }
        return false;
    };

    // A valid selection: the per-view depth is always declared, so the blit pass appears
    // (declared as a real reader - the lifetime extension is the graph's own dependency).
    {
        ViewSettings settings;
        settings.debug.resource = String(u8"forward.depth");
        frame.Begin(*h.encoder, 0);
        frame.AddView(scene, camera, settings, h.colorView, rhi::TextureFormat::BGRA8Unorm, 256,
                      256);
        frame.End();
        CHECK(hasPass(u8"debug.blit"));

        // The inventory lists the frame's textures, deduped, with the depth flag set.
        Array<DebugResourceInfo> rows;
        frame.CollectDebugResources(rows);
        bool foundDepth = false;
        for (const DebugResourceInfo& row : rows)
        {
            if (row.name == u8"forward.depth")
            {
                foundDepth = true;
                CHECK(row.isDepth);
                CHECK(row.width == 256);
                CHECK(row.height == 256);
            }
            // Transients only: the view's IMPORTED target must never list (selecting it
            // reads the blit's own write target - the forward.color layout-desync bug),
            // and undimensioned persistents (ibl.prefilter et al) can't be sampled.
            CHECK(row.name != u8"forward.color");
            CHECK(row.width > 0);
            CHECK(row.height > 0);
        }
        CHECK(foundDepth);
    }

    // An unknown name: no blit pass, no crash - the viewport just shows the final image.
    {
        ViewSettings settings;
        settings.debug.resource = String(u8"no.such.texture");
        frame.Begin(*h.encoder, 1);
        frame.AddView(scene, camera, settings, h.colorView, rhi::TextureFormat::BGRA8Unorm, 256,
                      256);
        frame.End();
        CHECK_FALSE(hasPass(u8"debug.blit"));
    }

    // The imported view target by name: filtered out (never a self-referential blit).
    {
        ViewSettings settings;
        settings.debug.resource = String(u8"forward.color");
        frame.Begin(*h.encoder, 1);
        frame.AddView(scene, camera, settings, h.colorView, rhi::TextureFormat::BGRA8Unorm, 256,
                      256);
        frame.End();
        CHECK_FALSE(hasPass(u8"debug.blit"));
    }

    // Selection off (empty resource): no blit pass either.
    {
        ViewSettings settings;
        frame.Begin(*h.encoder, 0);
        frame.AddView(scene, camera, settings, h.colorView, rhi::TextureFormat::BGRA8Unorm, 256,
                      256);
        frame.End();
        CHECK_FALSE(hasPass(u8"debug.blit"));
    }
}

TEST_CASE("debug view: semantic modes blit the raw scene HDR on the tonemap path only")
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

    TonemapPass tonemap(h.device, shaderSystem, /*framesInFlight*/ 2);
    REQUIRE(tonemap.Initialize().IsOk());
    DebugBlitPass debugBlit(h.device, shaderSystem, /*framesInFlight*/ 2);
    REQUIRE(debugBlit.Initialize().IsOk());

    RefPtr<geometry::StaticMesh> cube = geometry::Primitives::Cube(DefaultAllocator(), 1.0f);
    RefPtr<materials::Material> material =
        materials::MaterialBuilder(u8"lit").Shader(u8"forward").Build();
    ExtractedScene scene{DefaultAllocator()};
    MeshRenderData* rd = scene.Add<MeshRenderData>();
    rd->world = Float4x4::Identity();
    rd->mesh = cube.Get();
    rd->material = material.Get();
    rd->category = RenderCategories::Opaque;

    ViewCamera camera;
    camera.view = Float4x4::LookAtRH(Float3{0, 0, 5}, Float3{0, 0, 0}, Float3{0, 1, 0});
    camera.projection = Float4x4::PerspectiveFovRH(1.0472f, 1.0f, 0.1f, 100.0f);

    const auto hasPass = [](const RenderFrame& frame, StringView name)
    {
        for (const foundation::rendergraph::RenderGraphPass* pass : frame.Graph().Passes())
        {
            if (pass != nullptr && pass->name == name)
            {
                return true;
            }
        }
        return false;
    };

    // Tonemap path: the semantic term rides the scene HDR, so the raw blit appears
    // (tonemap would grade the encoded values).
    {
        RenderFrame frame(DefaultAllocator(), h.device, registry, /*framesInFlight*/ 2,
                          /*clusters*/ nullptr, &tonemap, /*shadows*/ nullptr,
                          /*ibl*/ nullptr, /*sky*/ nullptr, /*bloom*/ nullptr, /*taa*/ nullptr,
                          /*ao*/ nullptr, /*fxaa*/ nullptr, /*exposure*/ nullptr, &debugBlit);
        ViewSettings settings;
        settings.debug.semantic = ViewDebugSemantic::Albedo;
        frame.Begin(*h.encoder, 0);
        frame.AddView(scene, camera, settings, h.colorView, rhi::TextureFormat::BGRA8Unorm, 256,
                      256);
        frame.End();
        CHECK(hasPass(frame, u8"debug.blit"));
    }

    // No-tonemap path: the forward writes the LDR target directly, so the semantic values
    // are already raw on screen - no blit is (or should be) added.
    {
        RenderFrame frame(DefaultAllocator(), h.device, registry, /*framesInFlight*/ 2,
                          /*clusters*/ nullptr, /*tonemap*/ nullptr, /*shadows*/ nullptr,
                          /*ibl*/ nullptr, /*sky*/ nullptr, /*bloom*/ nullptr, /*taa*/ nullptr,
                          /*ao*/ nullptr, /*fxaa*/ nullptr, /*exposure*/ nullptr, &debugBlit);
        ViewSettings settings;
        settings.debug.semantic = ViewDebugSemantic::Albedo;
        frame.Begin(*h.encoder, 0);
        frame.AddView(scene, camera, settings, h.colorView, rhi::TextureFormat::BGRA8Unorm, 256,
                      256);
        frame.End();
        CHECK_FALSE(hasPass(frame, u8"debug.blit"));
    }
}

TEST_CASE("auto-exposure: per-(view,frame) bind-group slots survive consecutive frames, and a new scene snaps")
{
    // The exposure state PING-PONGS its prev-frame view, so a per-view-only slot
    // mismatched every frame and freed a descriptor set the previous frame's in-flight
    // command buffer still referenced (VUID-vkFreeDescriptorSets-00309 spam in the
    // editor with auto-exposure on). The slot now folds frameIndex in - this drives
    // the wired path across alternating frames; the validation-layer editor repro is
    // the on-device proof.
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

    TonemapPass tonemap(h.device, shaderSystem, /*framesInFlight*/ 2);
    REQUIRE(tonemap.Initialize().IsOk());
    ExposurePass exposure(h.device, shaderSystem, /*framesInFlight*/ 2);

    RenderFrame frame(DefaultAllocator(), h.device, registry, /*framesInFlight*/ 2,
                      /*clusters*/ nullptr, &tonemap, /*shadows*/ nullptr, /*ibl*/ nullptr,
                      /*sky*/ nullptr, /*bloom*/ nullptr, /*taa*/ nullptr, /*ao*/ nullptr,
                      /*fxaa*/ nullptr, &exposure);

    RefPtr<geometry::StaticMesh> cube = geometry::Primitives::Cube(DefaultAllocator(), 1.0f);
    RefPtr<materials::Material> material =
        materials::MaterialBuilder(u8"lit").Shader(u8"forward").Build();
    ExtractedScene scene{DefaultAllocator()};
    MeshRenderData* rd = scene.Add<MeshRenderData>();
    rd->world = Float4x4::Identity();
    rd->mesh = cube.Get();
    rd->material = material.Get();
    rd->category = RenderCategories::Opaque;

    ViewCamera camera;
    camera.view = Float4x4::LookAtRH(Float3{0, 0, 5}, Float3{0, 0, 0}, Float3{0, 1, 0});
    camera.projection = Float4x4::PerspectiveFovRH(1.0472f, 1.0f, 0.1f, 100.0f);
    ViewSettings settings;
    settings.post.autoExposure = true;

    const auto hasPass = [&](StringView name)
    {
        for (const foundation::rendergraph::RenderGraphPass* pass : frame.Graph().Passes())
        {
            if (pass != nullptr && pass->name == name)
            {
                return true;
            }
        }
        return false;
    };

    // Three frames = the ping-pong wraps and slot 0 gets REWRITTEN (frame 2 reuses
    // frame 0's slot); with the old per-view slot this rewrote every frame instead.
    // The first frame snaps to the measured value; the rest ease from the last frame.
    scene.SetSceneSerial(1);
    for (u32 f = 0; f < 3; ++f)
    {
        frame.SetDeltaSeconds(0.016f);
        frame.Begin(*h.encoder, f);
        frame.AddView(scene, camera, settings, h.colorView, rhi::TextureFormat::BGRA8Unorm, 128,
                      128);
        frame.End();
        CHECK(hasPass(u8"exposure.measure"));
        CHECK(exposure.Snapped(0) == (f == 0));
    }

    // Another scene in the same view (a level loaded) snaps again instead of easing from the
    // last scene's exposure (the dim and brighten at every level start), then eases as before.
    scene.SetSceneSerial(2);
    for (u32 f = 3; f < 5; ++f)
    {
        frame.SetDeltaSeconds(0.016f);
        frame.Begin(*h.encoder, f);
        frame.AddView(scene, camera, settings, h.colorView, rhi::TextureFormat::BGRA8Unorm, 128,
                      128);
        frame.End();
        CHECK(exposure.Snapped(0) == (f == 3));
    }
}

TEST_CASE("ssgi: enabling the per-view flag declares trace + resolve; the chain feeds SSR-less compose")
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

    TonemapPass tonemap(h.device, shaderSystem, /*framesInFlight*/ 2);
    REQUIRE(tonemap.Initialize().IsOk());
    SsgiPass ssgi(h.device, shaderSystem);
    REQUIRE(ssgi.Initialize().IsOk());

    RenderFrame frame(DefaultAllocator(), h.device, registry, /*framesInFlight*/ 2,
                      /*clusters*/ nullptr, &tonemap);
    frame.SetSsgi(&ssgi);

    RefPtr<geometry::StaticMesh> cube = geometry::Primitives::Cube(DefaultAllocator(), 1.0f);
    RefPtr<materials::Material> material =
        materials::MaterialBuilder(u8"lit").Shader(u8"forward").Build();
    ExtractedScene scene{DefaultAllocator()};
    MeshRenderData* rd = scene.Add<MeshRenderData>();
    rd->world = Float4x4::Identity();
    rd->mesh = cube.Get();
    rd->material = material.Get();
    rd->category = RenderCategories::Opaque;

    ViewCamera camera;
    camera.view = Float4x4::LookAtRH(Float3{0, 0, 5}, Float3{0, 0, 0}, Float3{0, 1, 0});
    camera.projection = Float4x4::PerspectiveFovRH(1.0472f, 1.0f, 0.1f, 100.0f);

    const auto hasPass = [&](StringView name)
    {
        for (const foundation::rendergraph::RenderGraphPass* pass : frame.Graph().Passes())
        {
            if (pass != nullptr && pass->name == name)
            {
                return true;
            }
        }
        return false;
    };

    // Enabled: both SSGI passes declare, and the graph textures land in the debug-view
    // inventory automatically (the layer-1 dividend).
    {
        ViewSettings settings;
        settings.post.ssgiEnabled = true;
        settings.post.ssgiIntensity = 1.0f;
        frame.Begin(*h.encoder, 0);
        frame.AddView(scene, camera, settings, h.colorView, rhi::TextureFormat::BGRA8Unorm, 128,
                      128);
        frame.End();
        CHECK(hasPass(u8"ssgi.down"));
        CHECK(hasPass(u8"ssgi.trace"));
        CHECK(hasPass(u8"ssgi.blur"));
        CHECK(hasPass(u8"ssgi.resolve"));

        Array<DebugResourceInfo> rows;
        frame.CollectDebugResources(rows);
        bool sawQuarter = false, sawRaw = false, sawFiltered = false, sawScene = false;
        for (const DebugResourceInfo& row : rows)
        {
            sawQuarter = sawQuarter || row.name == u8"ssgi.scene.quarter";
            sawRaw = sawRaw || row.name == u8"ssgi.raw";
            sawFiltered = sawFiltered || row.name == u8"ssgi.filtered";
            sawScene = sawScene || row.name == u8"ssgi.scene";
        }
        CHECK(sawQuarter);
        CHECK(sawRaw);
        CHECK(sawFiltered);
        CHECK(sawScene);
    }

    // Second frame: the temporal history ping-pongs without recreating (same size),
    // and the passes declare again.
    {
        ViewSettings settings;
        settings.post.ssgiEnabled = true;
        frame.Begin(*h.encoder, 1);
        frame.AddView(scene, camera, settings, h.colorView, rhi::TextureFormat::BGRA8Unorm, 128,
                      128);
        frame.End();
        CHECK(hasPass(u8"ssgi.trace"));
        CHECK(hasPass(u8"ssgi.resolve"));
    }

    // Disabled: no SSGI passes.
    {
        ViewSettings settings;
        frame.Begin(*h.encoder, 0);
        frame.AddView(scene, camera, settings, h.colorView, rhi::TextureFormat::BGRA8Unorm, 128,
                      128);
        frame.End();
        CHECK_FALSE(hasPass(u8"ssgi.trace"));
        CHECK_FALSE(hasPass(u8"ssgi.resolve"));
    }
}

TEST_CASE("mesh renderer: the per-frame rings hold the shadow casters, not only the camera's draws")
{
    // A town block seen down one street: the camera draws 150 meshes, the shadow cascades draw
    // all 330 casters (the camera does not cull them). Sized by the camera's draws, the shadows
    // filled the rings and the camera's own prepass and forward lost their meshes.
    const u32 cascades = 4;
    const u32 slots = MeshRenderer::InstanceSlotsPerFrame(150, 330, cascades, 0);
    CHECK(slots >= 150u * 2u + 330u * cascades);
    // Many draws and few casters (an interior): the camera passes still fit.
    CHECK(MeshRenderer::InstanceSlotsPerFrame(500, 20, cascades, 1) >= 500u * 3u + 20u * cascades);
    // Nothing to draw needs nothing.
    CHECK(MeshRenderer::InstanceSlotsPerFrame(0, 0, cascades, 0) == 0u);
}

// A blended material authored as data (blend mode set, depth left at ReadWrite) drew in the
// transparent pass with a depth-writing pipeline, which WebGPU refuses there; the pass reads depth only.
TEST_CASE("render: a pipeline for the transparent pass never writes depth")
{
    using foundation::materials::DepthMode;
    CHECK(MeshRenderer::TransparentPassDepth(DepthMode::ReadWrite) == DepthMode::ReadOnly);
    CHECK(MeshRenderer::TransparentPassDepth(DepthMode::WriteOnly) == DepthMode::ReadOnly);
    CHECK(MeshRenderer::TransparentPassDepth(DepthMode::ReadOnly) == DepthMode::ReadOnly);
    CHECK(MeshRenderer::TransparentPassDepth(DepthMode::Disabled) == DepthMode::Disabled);
}

// Far faded vegetation drew as flat grey silhouettes: the depth prepass left the camera position at
// the origin, so a set dissolved by its distance from there in depth but from the camera in colour.
// Every camera pass builds its context through CameraRecordContext, which carries the camera whole.
TEST_CASE("render: a camera pass's context carries the camera's position, matrix and scene clock")
{
    ExtractedScene scene{DefaultAllocator()};
    ViewCamera camera;
    camera.position = Float3{1200.0f, 80.0f, -640.0f};
    camera.view = Float4x4::LookAtRH(camera.position, Float3{1200.0f, 0.0f, -400.0f}, Float3{0, 1, 0});
    RenderView view;
    view.Bind(scene, camera, ViewSettings{}, nullptr, foundation::rhi::TextureFormat::RGBA8Unorm, 64, 64);

    RenderRecordContext ctx = CameraRecordContext(view, 3.0f, 2.5f);
    CHECK(ctx.view == &view);
    CHECK(ctx.cameraPos.x == 1200.0f);
    CHECK(ctx.cameraPos.y == 80.0f);
    CHECK(ctx.cameraPos.z == -640.0f);
    CHECK(ctx.viewMatrix.m[3][0] == camera.view.m[3][0]);
    CHECK(ctx.viewMatrix.m[3][2] == camera.view.m[3][2]);
    CHECK(ctx.timeSeconds == 3.0f); // no scene clock: the frame's
    CHECK(ctx.prevTimeSeconds == 2.5f);

    scene.SetTime(10.0f, 9.5f); // a scene clock wins
    ctx = CameraRecordContext(view, 3.0f, 2.5f);
    CHECK(ctx.timeSeconds == 10.0f);
    CHECK(ctx.prevTimeSeconds == 9.5f);
}

// A view's between-frame state follows its history key, not its place in the frame's list:
// PaperKid's minimap, a render texture drawn on alternate frames before the main view, moved the
// main view between places 0 and 1 every frame, so it read the minimap's camera and TAA history.
TEST_CASE("render: a view keeps its history slot by its key, wherever it falls in the frame")
{
    RenderHarness h;
    if (!h.Init(64, 64))
    {
        MESSAGE("DXC/Null unavailable; skipping");
        return;
    }
    shaders::ShaderSystem shaderSystem(*h.compiler, h.device);
    WireEngineShaders(shaderSystem);
    RendererRegistry registry;
    RenderFrame frame(DefaultAllocator(), h.device, registry, /*framesInFlight*/ 2);

    bool fresh = false;
    frame.Begin(*h.encoder, 0);
    const u32 main = frame.HistorySlotFor(1001, 0, fresh); // the main view, alone
    CHECK(fresh);
    frame.End();

    frame.Begin(*h.encoder, 1);
    const u32 side = frame.HistorySlotFor(2002, 0, fresh); // a render texture, drawn first
    CHECK(fresh);
    CHECK(side != main);
    CHECK(frame.HistorySlotFor(1001, 1, fresh) == main); // the main view, now second: same slot
    CHECK_FALSE(fresh);
    frame.End();

    frame.Begin(*h.encoder, 0);
    CHECK(frame.HistorySlotFor(1001, 0, fresh) == main); // first again, still the same slot
    CHECK_FALSE(fresh);
    frame.End();

    // Every slot taken: a new key takes the one seen longest ago (the side view's), never one in
    // use this frame, and is told it is fresh.
    for (u32 f = 0; f < RenderFrame::kHistorySlots; ++f)
    {
        frame.Begin(*h.encoder, f % 2);
        (void)frame.HistorySlotFor(1001, 0, fresh);
        for (u64 k = 0; k < RenderFrame::kHistorySlots - 2; ++k)
        {
            (void)frame.HistorySlotFor(5000 + k, static_cast<u32>(k + 1), fresh);
        }
        frame.End();
    }
    frame.Begin(*h.encoder, 0);
    (void)frame.HistorySlotFor(1001, 0, fresh);
    const u32 newcomer = frame.HistorySlotFor(9009, 1, fresh);
    CHECK(fresh);
    CHECK(newcomer == side);
    frame.End();
}
