// The scene->render extraction bridge: spawn a camera + mesh entities in a scene and
// verify ExtractSceneInto snapshots the per-mesh world matrices + mesh/material into an
// ExtractedScene (the data the scene-agnostic renderer consumes), and ExtractPrimaryCamera
// reads the camera view/projection.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"

import draconic.foundation;
import draconic.scene;
import draconic.geometry;
import draconic.materials;
import draconic.render;           // ExtractedScene / MeshRenderData / ViewCamera (scene-agnostic)
import draconic.engine.render; // components + ExtractSceneInto / ExtractPrimaryCamera
import draconic.scene.resource;   // SerializeScene (post-process settings round-trip)
import draconic.resource;
import draconic.rhi;
import draconic.rhi.null; // NullDevice (headless RenderSubsystem for the DebugView keying test)
import draconic.texture.resource; // texture::Texture (the sky-texture product)

using namespace draconic::foundation;
using namespace draconic::render;
namespace scene = draconic::scene;
namespace geometry = draconic::geometry;
namespace materials = draconic::materials;

namespace
{
    bool Near(f32 a, f32 b) { return Abs(a - b) < 1e-3f; }
}

TEST_CASE("ExtractSceneInto builds the draw list; ExtractPrimaryCamera reads the camera")
{
    scene::Scene scene(u8"world");
    auto* meshes = scene.AddSystem<MeshComponentManager>();
    auto* cameras = scene.AddSystem<CameraComponentManager>();

    scene::EntityHandle camEntity = scene.CreateEntity(u8"camera");
    scene.SetLocalPosition(camEntity, Float3{0, 0, 5});
    CameraComponent& cam = cameras->Add(camEntity);
    cam.aspect = 1.0f;

    RefPtr<geometry::StaticMesh> cube = geometry::Primitives::Cube(1.0f);
    RefPtr<materials::Material> material =
        materials::MaterialBuilder(u8"lit").Shader(u8"forward").Build();

    scene::EntityHandle a = scene.CreateEntity(u8"a");
    scene.SetLocalPosition(a, Float3{-2, 0, 0});
    {
        MeshComponent& m = meshes->Add(a);
        m.mesh = cube;
        m.SetMaterial(material);
        m.color = Color{0.2f, 0.4f, 0.8f, 1.0f};
    }

    scene::EntityHandle b = scene.CreateEntity(u8"b");
    scene.SetLocalPosition(b, Float3{3, 0, 0});
    {
        MeshComponent& m = meshes->Add(b);
        m.mesh = cube;
        m.SetMaterial(material);
    }

    scene.UpdateTransforms();

    ViewCamera vc;
    REQUIRE(ExtractPrimaryCamera(scene, vc));
    CHECK(Near(vc.view.m[3][2], -5.0f));            // view = inverse(camera world)
    CHECK_FALSE(Near(vc.projection.m[2][3], 0.0f)); // a real perspective projection

    ExtractedScene snapshot;
    ExtractSceneInto(scene, snapshot);
    REQUIRE(snapshot.Size() == 2);

    f32 sumX = 0.0f;
    bool sawBlue = false;
    for (RenderData* rd : snapshot.Items())
    {
        const auto* m = static_cast<const MeshRenderData*>(rd);
        CHECK(m->category == RenderCategories::Opaque); // opaque material -> opaque category
        CHECK(m->mesh == cube.Get());
        CHECK(m->material == material.Get());
        CHECK(m->entityId != 0); // tagged with a packed entity handle
        sumX += m->world.m[3][0];
        if (Near(m->color.b, 0.8f))
        {
            sawBlue = true;
        } // per-instance color carried through
    }
    CHECK(Near(sumX, 1.0f)); // -2 + 3
    CHECK(sawBlue);
}

TEST_CASE("ExtractSceneInto skips invisible + mesh-less components; no primary camera reported")
{
    scene::Scene scene;
    auto* meshes = scene.AddSystem<MeshComponentManager>();
    auto* cameras = scene.AddSystem<CameraComponentManager>();

    RefPtr<geometry::StaticMesh> mesh = geometry::Primitives::Quad();

    scene::EntityHandle visible = scene.CreateEntity();
    {
        MeshComponent& m = meshes->Add(visible);
        m.mesh = mesh;
    }

    scene::EntityHandle hidden = scene.CreateEntity();
    {
        MeshComponent& m = meshes->Add(hidden);
        m.mesh = mesh;
        m.visible = false;
    }

    meshes->Add(scene.CreateEntity()); // no mesh assigned

    scene::EntityHandle cam2 = scene.CreateEntity();
    {
        CameraComponent& c = cameras->Add(cam2);
        c.primary = false;
    }

    scene.UpdateTransforms();

    ViewCamera vc;
    CHECK_FALSE(ExtractPrimaryCamera(scene, vc)); // no primary camera

    ExtractedScene snapshot;
    ExtractSceneInto(scene, snapshot);
    REQUIRE(snapshot.Size() == 1); // only the visible, meshed one
}

TEST_CASE("ExtractSceneInto on a scene without render managers yields an empty snapshot")
{
    scene::Scene scene;
    scene.CreateEntity();
    scene.UpdateTransforms();

    ViewCamera vc;
    CHECK_FALSE(ExtractPrimaryCamera(scene, vc));

    ExtractedScene snapshot;
    ExtractSceneInto(scene, snapshot);
    CHECK(snapshot.Size() == 0);
}

namespace
{
    // Builds a scene of `n` quad meshes at world x = 0..n-1; returns the mesh/material alive.
    void BuildBigScene(scene::Scene& scene, int n, RefPtr<geometry::StaticMesh>& mesh,
                       RefPtr<materials::Material>& material)
    {
        auto* meshes = scene.AddSystem<MeshComponentManager>();
        mesh = geometry::Primitives::Quad();
        material = materials::MaterialBuilder(u8"lit").Shader(u8"forward").Build();
        for (int i = 0; i < n; ++i)
        {
            scene::EntityHandle e = scene.CreateEntity();
            scene.SetLocalPosition(e, Float3{static_cast<f32>(i), 0, 0});
            MeshComponent& m = meshes->Add(e);
            m.mesh = mesh;
            m.SetMaterial(material);
        }
        scene.UpdateTransforms();
    }
    // The world-x sum is a drop/dup-proof invariant: each entity contributes its index exactly once.
    f64 SumWorldX(const ExtractedScene& s)
    {
        f64 sum = 0;
        for (RenderData* rd : s.Items())
        {
            sum += static_cast<MeshRenderData*>(rd)->world.m[3][0];
        }
        return sum;
    }
}

TEST_CASE("ExtractSceneInto (parallel) extracts every renderable exactly once")
{
    InitGlobalJobSystem(4);
    {
        constexpr int N = 2000; // > kParallelExtractThreshold
        scene::Scene scene;
        RefPtr<geometry::StaticMesh> mesh;
        RefPtr<materials::Material> material;
        BuildBigScene(scene, N, mesh, material);

        RenderContext ctx;
        ctx.BeginFrame(GlobalJobs().SlotCount());
        ExtractedScene out;
        ExtractSceneInto(scene, out, ctx); // takes the parallel path

        REQUIRE(out.Size() == static_cast<usize>(N)); // no drops, no duplicates
        CHECK(SumWorldX(out) == static_cast<f64>(N) * (N - 1) / 2.0);
    }
    ShutdownGlobalJobSystem();
}

TEST_CASE("ExtractSceneInto (ctx) falls back to serial with no job system")
{
    REQUIRE_FALSE(HasGlobalJobSystem()); // none started in this test binary
    scene::Scene scene;
    RefPtr<geometry::StaticMesh> mesh;
    RefPtr<materials::Material> material;
    BuildBigScene(scene, 50, mesh, material);

    RenderContext ctx;
    ctx.BeginFrame(1);
    ExtractedScene out;
    ExtractSceneInto(scene, out, ctx);

    REQUIRE(out.Size() == 50u);
    CHECK(SumWorldX(out) == static_cast<f64>(50) * 49 / 2.0);
}

TEST_CASE("ExtractSceneInto maps a transparent material to the Transparent category")
{
    scene::Scene scene;
    auto* meshes = scene.AddSystem<MeshComponentManager>();

    RefPtr<geometry::StaticMesh> mesh = geometry::Primitives::Quad();
    RefPtr<materials::Material> glass =
        materials::MaterialBuilder(u8"glass").Shader(u8"forward").Transparent().Build();

    scene::EntityHandle e = scene.CreateEntity();
    {
        MeshComponent& m = meshes->Add(e);
        m.mesh = mesh;
        m.SetMaterial(glass);
    }
    scene.UpdateTransforms();

    ExtractedScene snapshot;
    ExtractSceneInto(scene, snapshot);
    REQUIRE(snapshot.Size() == 1);
    const auto* md = static_cast<const MeshRenderData*>(snapshot.Items()[0]);
    CHECK(md->category == RenderCategories::Transparent);
}

TEST_CASE("instanced-mesh: seeded identity instance + entity-relative composition")
{
    scene::Scene scene(u8"world");
    auto* mgr = scene.AddSystem<InstancedMeshComponentManager>();
    RefPtr<geometry::StaticMesh> cube = geometry::Primitives::Cube(1.0f);

    // A fresh component is seeded with ONE identity instance (editor workflow: assign a mesh,
    // see it render at the entity), and instances compose with the entity's world transform.
    scene::EntityHandle e = scene.CreateEntity(u8"scatter");
    scene.SetLocalPosition(e, Float3{5, 0, 0});
    InstancedMeshComponent& c = mgr->Add(e);
    REQUIRE(c.Count() == 1u);
    c.mesh = cube;
    scene.UpdateTransforms();

    ExtractedScene out;
    ExtractInstancedMeshesInto(scene, out);
    REQUIRE(out.Items().Size() == 1u);
    const auto* rd = static_cast<const MultiMeshRenderData*>(out.Items()[0]);
    REQUIRE(rd->instanceCount == 1u);
    CHECK(Near(rd->transforms[0].m[3][0], 5.0f)); // identity instance * entity world
    CHECK(Near(rd->worldCenter.x, 5.0f));
    const u32 firstVersion = rd->version;

    // Moving the ENTITY moves the set: the composed transforms change and the renderer's upload
    // key (version) bumps even though the authored set didn't change.
    scene.SetLocalPosition(e, Float3{5, 7, 0});
    scene.UpdateTransforms();
    ExtractedScene out2;
    ExtractInstancedMeshesInto(scene, out2);
    const auto* rd2 = static_cast<const MultiMeshRenderData*>(out2.Items()[0]);
    CHECK(Near(rd2->transforms[0].m[3][1], 7.0f));
    CHECK(rd2->version != firstVersion);

    // Unmoved + unchanged: no recompose, same version (static sets stay zero-cost).
    ExtractedScene out3;
    ExtractInstancedMeshesInto(scene, out3);
    const auto* rd3 = static_cast<const MultiMeshRenderData*>(out3.Items()[0]);
    CHECK(rd3->version == rd2->version);

    // Authored instances are entity-relative: replace the seed with two local offsets.
    const Float4x4 xf[2] = {Float4x4::Translation(Float3{1, 0, 0}),
                            Float4x4::Translation(Float3{-1, 0, 0})};
    c.SetInstances(Span<const Float4x4>{xf, 2});
    ExtractedScene out4;
    ExtractInstancedMeshesInto(scene, out4);
    const auto* rd4 = static_cast<const MultiMeshRenderData*>(out4.Items()[0]);
    REQUIRE(rd4->instanceCount == 2u);
    CHECK(Near(rd4->transforms[0].m[3][0], 6.0f)); // 1 + entity x=5
    CHECK(Near(rd4->transforms[1].m[3][0], 4.0f)); // -1 + entity x=5
}

TEST_CASE("ExtractEnvironmentInto carries the sky texture product (uid identity, cube flag)")
{
    draconic::scene::Scene scene(u8"s");
    auto* env = scene.AddSystem<draconic::render::EnvironmentSystem>();
    env->Environment().skyMode = draconic::render::SkyMode::Cubemap;

    // A cube-shaped product (no GPU objects needed - identity/shape are what extraction reads).
    RefPtr<draconic::texture::Texture> sky =
        MakeRef<draconic::texture::Texture>(DefaultAllocator());
    sky->Adopt(nullptr, nullptr, nullptr, nullptr, 64, 64, rhi::TextureFormat::RGBA8Unorm,
               /*isCube*/ true);
    env->Environment().skyTexture =
        sky.Get(); // direct override (picker/serialized path binds by guid)

    draconic::render::ExtractedScene out;
    draconic::render::ExtractEnvironmentInto(scene, out);
    CHECK(out.Sky().mode == draconic::render::SkyMode::Cubemap);
    CHECK(out.Sky().textureUid == sky->Uid());
    CHECK(out.Sky().textureUid != 0u);
    CHECK(out.Sky().textureIsCube);

    // No texture -> no identity (the IBL keeps its programmatic/procedural source).
    env->Environment().skyTexture = draconic::resource::Ref<draconic::texture::Texture>{};
    draconic::render::ExtractedScene out2;
    draconic::render::ExtractEnvironmentInto(scene, out2);
    CHECK(out2.Sky().textureUid == 0u);
}

TEST_CASE("extraction refreshes the material cache from the refs EVERY frame (late binds heal)")
{
    // The Sponza symptom: multi-materials that resolve AFTER the first frame (cook finishing
    // in the background) used to stay null forever - the cache was a one-shot resolve-time
    // snapshot. Extraction now re-reads the refs per frame.
    scene::Scene scene(u8"world");
    auto* meshes = scene.AddSystem<MeshComponentManager>();

    RefPtr<geometry::StaticMesh> cube = geometry::Primitives::Cube(1.0f);
    RefPtr<materials::Material> matA =
        materials::MaterialBuilder(u8"a").Shader(u8"forward").Build();
    RefPtr<materials::Material> matB =
        materials::MaterialBuilder(u8"b").Shader(u8"forward").Build();

    scene::EntityHandle e = scene.CreateEntity(u8"multi");
    MeshComponent& mc = meshes->Add(e);
    mc.mesh = cube;
    // Two slots: slot 0 resolved, slot 1 UNRESOLVED (a bare guid - the pre-cook state).
    mc.materials.PushBack(draconic::resource::Ref<materials::Material>(matA));
    draconic::resource::Ref<materials::Material> late;
    late.SetId(Guid{0x1, 0x2});
    mc.materials.PushBack(late);

    ExtractedScene first;
    ExtractSceneInto(scene, first);
    REQUIRE(first.Items().Size() == 1u);
    {
        const auto* rd = static_cast<const MeshRenderData*>(first.Items()[0]);
        CHECK(rd->material == matA.Get()); // slot 0 = the whole-mesh primary
        REQUIRE(rd->submeshMaterialCount == 2u);
        CHECK(rd->submeshMaterials[1].Get() == nullptr); // not cooked yet
    }

    // "The cook lands": the slot resolves (direct adopt stands in for the proxy binding).
    mc.materials[1].SetDirect(RefPtr<materials::Material>(matB.Get()));

    ExtractedScene second;
    ExtractSceneInto(scene, second);
    REQUIRE(second.Items().Size() == 1u);
    {
        const auto* rd = static_cast<const MeshRenderData*>(second.Items()[0]);
        CHECK(rd->submeshMaterials[1].Get() == matB.Get()); // healed - no reopen needed
    }

    // Single-entry list = whole-mesh path (no submesh routing), serving the old single-
    // material setup through the same array.
    mc.materials.Resize(1);
    ExtractedScene third;
    ExtractSceneInto(scene, third);
    {
        const auto* rd = static_cast<const MeshRenderData*>(third.Items()[0]);
        CHECK(rd->material == matA.Get());
        CHECK(rd->submeshMaterialCount == 0u);
    }
}

TEST_CASE("extract: postTonemap sprites land in the WorldUI category (authored colors)")
{
    scene::Scene scene{u8"world"};
    scene.AddSystem<SpriteComponentManager>();
    const scene::EntityHandle e = scene.CreateEntity(u8"panel");
    SpriteComponent& sprite = scene.GetSystem<SpriteComponentManager>()->Add(e);
    rhi::TextureView* fakeView = reinterpret_cast<rhi::TextureView*>(0x1); // extract only stores it
    sprite.texture = fakeView;
    sprite.postTonemap = true;
    sprite.orientation = SpriteOrientation::EntityOriented;
    scene.UpdateTransforms();

    ExtractedScene out;
    ExtractSpritesInto(scene, out, /*rendererId*/ 1);
    REQUIRE(out.Items().Size() == 1u);
    const auto* rd = static_cast<const SpriteRenderData*>(out.Items()[0]);
    CHECK(rd->category == RenderCategories::WorldUI);
    CHECK(rd->postTonemap);
    CHECK(Categories().Affinity(rd->category) == PassAffinity::PostTonemap);
    CHECK(Categories().Sort(rd->category) == SortMode::BackToFront);

    // The default path stays in Transparent.
    sprite.postTonemap = false;
    ExtractedScene plain;
    ExtractSpritesInto(scene, plain, 1);
    REQUIRE(plain.Items().Size() == 1u);
    CHECK(plain.Items()[0]->category == RenderCategories::Transparent);
}

TEST_CASE("PostProcessSettings: defaults match today's look, and round-trip through the scene")
{
    RegisterRenderComponentReflection();

    // Defaults MUST match the RenderSubsystem's current hardcoded values (Phase 1 changes nothing
    // visually until the passes are wired to read these).
    {
        PostProcessSystem sys;
        const PostProcessSettings& d = sys.Post();
        CHECK(d.exposureEV == 0.0f); // 2^0 = the old fixed 1.0 multiplier
        CHECK(d.tonemapOperator == TonemapOperator::AgX);
        CHECK(d.bloomEnabled);
        CHECK(Near(d.bloomIntensity, 0.05f));
        CHECK(Near(d.bloomThreshold, 1.0f));
        CHECK(Near(d.bloomKnee, 0.6f));
        CHECK(d.aoMode == AoMode::Off);
        CHECK(Near(d.aoStrength, 0.6f));
        CHECK(d.ssrEnabled == false);
        CHECK(d.aaMode == AaMode::Off);
        CHECK(Near(d.taaBlendFactor, 0.97f));
        CHECK(Near(d.taaVarianceGamma, 1.25f));
    }

    // Reflected for the auto-generated inspector section: the type resolves with every property.
    const TypeInfo& ti = TypeOf<PostProcessSettings>();
    CHECK(PropertyCount(ti) >= 15u); // exposure + tonemap + bloom(4) + ao(4) + ssr(2) + aa(4)

    // Edit a field of each kind (float, bool, all three enums), serialize the whole scene, reload:
    // the authored look survives (the "serialize with the scene, ship to the runtime" contract).
    scene::Scene a(u8"look");
    PostProcessSystem* postA = a.AddSystem<PostProcessSystem>();
    postA->Post().exposureEV = 1.5f;
    postA->Post().tonemapOperator = TonemapOperator::Clamp;
    postA->Post().bloomIntensity = 0.2f;
    postA->Post().aoMode = AoMode::GTAO;
    postA->Post().ssrEnabled = true;
    postA->Post().aaMode = AaMode::TAA;
    postA->Post().taaBlendFactor = 0.9f;
    (void)a.CreateEntity(u8"e");

    MemoryStream stream;
    {
        BinarySerializer writer(stream, SerializeMode::Write);
        SerializeScene(writer, a);
        REQUIRE(writer.IsOk());
    }
    (void)stream.Seek(0, SeekOrigin::Begin);
    scene::Scene b;
    PostProcessSystem* postB = b.AddSystem<PostProcessSystem>();
    {
        BinarySerializer reader(stream, SerializeMode::Read);
        SerializeScene(reader, b, &stream);
        REQUIRE(reader.IsOk());
    }
    const PostProcessSettings& r = postB->Post();
    CHECK(Near(r.exposureEV, 1.5f));
    CHECK(r.tonemapOperator == TonemapOperator::Clamp);
    CHECK(Near(r.bloomIntensity, 0.2f));
    CHECK(r.aoMode == AoMode::GTAO);
    CHECK(r.ssrEnabled);
    CHECK(r.aaMode == AaMode::TAA);
    CHECK(Near(r.taaBlendFactor, 0.9f));
}

TEST_CASE("ResolveScenePost maps authored settings to the per-view ViewPostConfig (EV -> linear)")
{
    // A default block resolves to the historical renderer globals (no visual change).
    {
        PostProcessSettings d;
        const ViewPostConfig vp = ResolveScenePost(d);
        CHECK(Near(vp.exposure, 1.0f)); // 2^0
        CHECK(vp.bloomEnabled);
        CHECK(Near(vp.bloomIntensity, 0.05f));
        CHECK(vp.aoMode == 0u); // AoMode::Off
        CHECK(Near(vp.aoStrength, 0.6f));
    }
    // Exposure is authored in stops: +2 EV = 4x, -1 EV = 0.5x. AO enum -> u32.
    {
        PostProcessSettings s;
        s.exposureEV = 2.0f;
        const ViewPostConfig vp = ResolveScenePost(s);
        CHECK(Near(vp.exposure, 4.0f));
    }
    {
        PostProcessSettings s;
        s.exposureEV = -1.0f;
        s.aoMode = AoMode::SSAO;
        s.bloomEnabled = false;
        const ViewPostConfig vp = ResolveScenePost(s);
        CHECK(Near(vp.exposure, 0.5f));
        CHECK(vp.aoMode == 2u); // AoMode::SSAO
        CHECK_FALSE(vp.bloomEnabled);
    }
    // AA-mode enum -> the mutually-exclusive TAA/FXAA flags; tonemap operator -> agx bool; SSR.
    {
        PostProcessSettings taa;
        taa.aaMode = AaMode::TAA;
        taa.taaBlendFactor = 0.9f;
        const ViewPostConfig vp = ResolveScenePost(taa);
        CHECK(vp.taaEnabled);
        CHECK_FALSE(vp.fxaaEnabled);
        CHECK(Near(vp.taaBlend, 0.9f));
        CHECK(vp.needsMotion); // TAA needs motion vectors
    }
    {
        PostProcessSettings fx;
        fx.aaMode = AaMode::FXAA;
        const ViewPostConfig vp = ResolveScenePost(fx);
        CHECK(vp.fxaaEnabled);
        CHECK_FALSE(vp.taaEnabled);
        CHECK_FALSE(vp.needsMotion); // FXAA is post-tonemap, no motion
    }
    {
        PostProcessSettings tm;
        tm.tonemapOperator = TonemapOperator::Clamp;
        CHECK_FALSE(ResolveScenePost(tm).agxTonemap);
        PostProcessSettings agx;
        agx.tonemapOperator = TonemapOperator::AgX;
        CHECK(ResolveScenePost(agx).agxTonemap);
    }
    {
        PostProcessSettings ssr;
        ssr.ssrEnabled = true;
        ssr.ssrIntensity = 0.7f;
        const ViewPostConfig vp = ResolveScenePost(ssr);
        CHECK(vp.ssrEnabled);
        CHECK(Near(vp.ssrIntensity, 0.7f));
    }
}

TEST_CASE("ApplyViewPostOverride strips effects per view without touching the authored config")
{
    // Start from a fully-enabled config (exposure/tonemap preserved by every override).
    const auto base = []()
    {
        ViewPostConfig vp;
        vp.bloomEnabled = true;
        vp.aoMode = 1u;
        vp.ssrEnabled = true;
        vp.taaEnabled = true;
        vp.fxaaEnabled = false;
        vp.exposure = 2.0f;
        return vp;
    };

    // A single flag strips just its effect.
    {
        ViewPostConfig vp = base();
        ApplyViewPostOverride(vp, ViewPostOverride{.disableBloom = true});
        CHECK_FALSE(vp.bloomEnabled);
        CHECK(vp.aoMode == 1u);
    }
    {
        ViewPostConfig vp = base();
        ApplyViewPostOverride(vp, ViewPostOverride{.disableAo = true});
        CHECK(vp.aoMode == 0u);
        CHECK(vp.bloomEnabled);
    }
    {
        ViewPostConfig vp = base();
        ApplyViewPostOverride(vp, ViewPostOverride{.disableSsr = true});
        CHECK_FALSE(vp.ssrEnabled);
    }
    {
        ViewPostConfig vp = base();
        ApplyViewPostOverride(vp, ViewPostOverride{.disableAa = true});
        CHECK_FALSE(vp.taaEnabled);
        CHECK_FALSE(vp.fxaaEnabled);
    }

    // The master "No Post" strips bloom/AO/SSR/AA but keeps exposure + tonemap (so it still displays).
    {
        ViewPostConfig vp = base();
        ApplyViewPostOverride(vp, ViewPostOverride{.disablePost = true});
        CHECK_FALSE(vp.bloomEnabled);
        CHECK(vp.aoMode == 0u);
        CHECK_FALSE(vp.ssrEnabled);
        CHECK_FALSE(vp.taaEnabled);
        CHECK(Near(vp.exposure, 2.0f)); // exposure preserved
    }
    // A default (empty) override changes nothing.
    {
        ViewPostConfig vp = base();
        ApplyViewPostOverride(vp, ViewPostOverride{});
        CHECK(vp.bloomEnabled);
        CHECK(vp.ssrEnabled);
        CHECK(vp.taaEnabled);
    }
}

TEST_CASE("components: reflected types carry authored displayName + category attributes")
{
    // The inspector's add-component menu and section headers resolve these; an annotated
    // type must expose BOTH (editor-polish.md P1 - authored intent, not name heuristics).
    RegisterRenderComponentReflection();

    const struct
    {
        const TypeInfo* type;
        StringView displayName;
    } expectations[] = {
        {&TypeOf<MeshComponent>(), u8"Mesh"},
        {&TypeOf<InstancedMeshComponent>(), u8"Instanced Mesh"},
        {&TypeOf<CameraComponent>(), u8"Camera"},
        {&TypeOf<LightComponent>(), u8"Light"},
        {&TypeOf<SpriteComponent>(), u8"Sprite"},
        {&TypeOf<DecalComponent>(), u8"Decal"},
        {&TypeOf<ReflectionProbeComponent>(), u8"Reflection Probe"},
        {&TypeOf<PostProcessSettings>(), u8"Post Processing"},
    };
    for (const auto& expectation : expectations)
    {
        const Variant* display = FindAttribute(*expectation.type, "displayName");
        REQUIRE(display != nullptr);
        const String* name = display->TryGet<String>();
        REQUIRE(name != nullptr);
        CHECK(name->AsView() == expectation.displayName);

        const Variant* category = FindAttribute(*expectation.type, "category");
        REQUIRE(category != nullptr);
        const String* categoryName = category->TryGet<String>();
        REQUIRE(categoryName != nullptr);
        CHECK(categoryName->AsView() == u8"Rendering");
    }
}

TEST_CASE("render: DebugView isolates per-view gizmos (camera-preview fix, task #118)")
{
    rhi::null::NullDevice device{DefaultAllocator()};
    RenderSubsystem sub{device, 2};

    // Two distinct viewport keys (e.g. the main editor viewport + the camera-preview inset).
    int mainKey = 0;
    int previewKey = 0;

    // Distinct keys -> distinct, STABLE buffers; the same key always returns the same buffer.
    debug::DebugDraw& mainDbg = sub.DebugView(&mainKey);
    debug::DebugDraw& previewDbg = sub.DebugView(&previewKey);
    CHECK(&mainDbg != &previewDbg);
    CHECK(&sub.DebugView(&mainKey) == &mainDbg);
    CHECK(&sub.DebugView(&previewKey) == &previewDbg);

    // A per-view buffer is also distinct from the shared per-scene buffer path.
    CHECK(&sub.DebugView(&mainKey) != &sub.DebugGlobal());

    // Draw into the MAIN view's buffer; the PREVIEW view's buffer stays empty - so an editor
    // viewport's grid/gizmos can never bleed into a second view of the same scene.
    mainDbg.DrawLine(Float3{0, 0, 0}, Float3{1, 0, 0}, Color{1, 1, 1, 1});
    CHECK(mainDbg.HasAnyDraws());
    CHECK_FALSE(previewDbg.HasAnyDraws());
}
