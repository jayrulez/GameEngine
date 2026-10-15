// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The scene->render extraction bridge: spawn a camera + mesh entities in a scene and
// verify ExtractSceneInto snapshots the per-mesh world matrices + mesh/material into an
// ExtractedScene (the data the scene-agnostic renderer consumes), and ExtractPrimaryCamera
// reads the camera view/projection.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.vfs; // the data mount the subsystem reads engine data through
import foundation.scene;
import foundation.geometry;
import foundation.materials;
import foundation.render;           // ExtractedScene / MeshRenderData / ViewCamera (scene-agnostic)
import engine.render; // components + ExtractSceneInto / ExtractPrimaryCamera
import foundation.scene.resource;   // SerializeScene (post-process settings round-trip)
import foundation.resource;
import foundation.rhi;
import foundation.rhi.null; // NullDevice (headless RenderSubsystem for the DebugView keying test)
import foundation.texture.resource; // texture::Texture (the sky-texture product)
import foundation.xml;               // XmlDocument (a member's record)
import foundation.xml.serialization; // XmlSerializer

using namespace foundation::core;
using namespace engine::render;
using namespace foundation::render;
namespace rhi = foundation::rhi;
namespace scene = foundation::scene;
namespace geometry = foundation::geometry;
namespace materials = foundation::materials;

namespace
{
    bool Near(f32 a, f32 b) { return Abs(a - b) < 1e-3f; }
}

TEST_CASE("ExtractSceneInto builds the draw list; ExtractPrimaryCamera reads the camera")
{
    scene::Scene scene(DefaultAllocator(), u8"world");
    auto* meshes = scene.AddSystem<MeshComponentManager>();
    auto* cameras = scene.AddSystem<CameraComponentManager>();

    scene::EntityHandle camEntity = scene.CreateEntity(u8"camera");
    scene.SetLocalPosition(camEntity, Float3{0, 0, 5});
    CameraComponent& cam = cameras->Add(camEntity);
    cam.aspect = 1.0f;

    RefPtr<geometry::StaticMesh> cube = geometry::Primitives::Cube(DefaultAllocator(), 1.0f);
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
    // The projection takes the shape the view draws into when given one, the authored aspect
    // otherwise: x scale = y scale / aspect (Sedulous ebbf7a3b).
    const f32 authored = vc.projection.m[1][1] / vc.projection.m[0][0];
    ViewCamera wide;
    REQUIRE(ExtractPrimaryCamera(scene, wide, nullptr, 3.0f));
    CHECK(Near(wide.projection.m[1][1] / wide.projection.m[0][0], 3.0f)); // the view's aspect
    CHECK_FALSE(Near(authored, 3.0f));                                    // not the authored one

    ExtractedScene snapshot{DefaultAllocator()};
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
        if (Near(m->color.b, SrgbToLinear(0.8f)))
        {
            sawBlue = true;
        } // per-instance color carried through, decoded to linear
    }
    CHECK(Near(sumX, 1.0f)); // -2 + 3
    CHECK(sawBlue);
}

TEST_CASE("ExtractSceneInto skips invisible + mesh-less components; no primary camera reported")
{
    scene::Scene scene{DefaultAllocator()};
    auto* meshes = scene.AddSystem<MeshComponentManager>();
    auto* cameras = scene.AddSystem<CameraComponentManager>();

    RefPtr<geometry::StaticMesh> mesh = geometry::Primitives::Quad(DefaultAllocator());

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

    ExtractedScene snapshot{DefaultAllocator()};
    ExtractSceneInto(scene, snapshot);
    REQUIRE(snapshot.Size() == 1); // only the visible, meshed one
}

TEST_CASE("ExtractSceneInto on a scene without render managers yields an empty snapshot")
{
    scene::Scene scene{DefaultAllocator()};
    scene.CreateEntity();
    scene.UpdateTransforms();

    ViewCamera vc;
    CHECK_FALSE(ExtractPrimaryCamera(scene, vc));

    ExtractedScene snapshot{DefaultAllocator()};
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
        mesh = geometry::Primitives::Quad(DefaultAllocator());
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
        scene::Scene scene{DefaultAllocator()};
        RefPtr<geometry::StaticMesh> mesh;
        RefPtr<materials::Material> material;
        BuildBigScene(scene, N, mesh, material);

        RenderContext ctx{DefaultAllocator()};
        ctx.BeginFrame(GlobalJobs().SlotCount());
        ExtractedScene out{DefaultAllocator()};
        ExtractSceneInto(scene, out, ctx); // takes the parallel path

        REQUIRE(out.Size() == static_cast<usize>(N)); // no drops, no duplicates
        CHECK(SumWorldX(out) == static_cast<f64>(N) * (N - 1) / 2.0);
    }
    ShutdownGlobalJobSystem();
}

TEST_CASE("ExtractSceneInto (ctx) falls back to serial with no job system")
{
    REQUIRE_FALSE(HasGlobalJobSystem()); // none started in this test binary
    scene::Scene scene{DefaultAllocator()};
    RefPtr<geometry::StaticMesh> mesh;
    RefPtr<materials::Material> material;
    BuildBigScene(scene, 50, mesh, material);

    RenderContext ctx{DefaultAllocator()};
    ctx.BeginFrame(1);
    ExtractedScene out{DefaultAllocator()};
    ExtractSceneInto(scene, out, ctx);

    REQUIRE(out.Size() == 50u);
    CHECK(SumWorldX(out) == static_cast<f64>(50) * 49 / 2.0);
}

TEST_CASE("ExtractSceneInto maps a transparent material to the Transparent category")
{
    scene::Scene scene{DefaultAllocator()};
    auto* meshes = scene.AddSystem<MeshComponentManager>();

    RefPtr<geometry::StaticMesh> mesh = geometry::Primitives::Quad(DefaultAllocator());
    RefPtr<materials::Material> glass =
        materials::MaterialBuilder(u8"glass").Shader(u8"forward").Transparent().Build();

    scene::EntityHandle e = scene.CreateEntity();
    {
        MeshComponent& m = meshes->Add(e);
        m.mesh = mesh;
        m.SetMaterial(glass);
    }
    scene.UpdateTransforms();

    ExtractedScene snapshot{DefaultAllocator()};
    ExtractSceneInto(scene, snapshot);
    REQUIRE(snapshot.Size() == 1);
    const auto* md = static_cast<const MeshRenderData*>(snapshot.Items()[0]);
    CHECK(md->category == RenderCategories::Transparent);
}

TEST_CASE("ExtractSceneInto carries a mesh's fade, clamped, and draws a faded opaque mesh Masked")
{
    scene::Scene scene{DefaultAllocator()};
    auto* meshes = scene.AddSystem<MeshComponentManager>();

    RefPtr<geometry::StaticMesh> mesh = geometry::Primitives::Quad(DefaultAllocator());
    RefPtr<materials::Material> plaster =
        materials::MaterialBuilder(u8"plaster").Shader(u8"forward").Build();
    RefPtr<materials::Material> glass =
        materials::MaterialBuilder(u8"glass").Shader(u8"forward").Transparent().Build();

    // Solid, half faded, over-faded (clamps to 1), and a faded transparent one (stays Transparent).
    const f32 fades[] = {0.0f, 0.5f, 3.0f, 0.5f};
    for (u32 i = 0; i < 4; ++i)
    {
        MeshComponent& m = meshes->Add(scene.CreateEntity());
        m.mesh = mesh;
        m.SetMaterial(i == 3 ? glass : plaster);
        m.fade = fades[i];
    }
    scene.UpdateTransforms();

    ExtractedScene snapshot{DefaultAllocator()};
    ExtractSceneInto(scene, snapshot);
    REQUIRE(snapshot.Size() == 4);
    u32 solid = 0, half = 0, gone = 0, glassy = 0;
    for (usize i = 0; i < snapshot.Size(); ++i)
    {
        const auto* md = static_cast<const MeshRenderData*>(snapshot.Items()[i]);
        if (md->category == RenderCategories::Transparent)
        {
            glassy += Near(md->fade, 0.5f) ? 1u : 0u;
        }
        else if (md->fade == 0.0f)
        {
            // Solid: the depth prepass draws it, as before.
            solid += md->category == RenderCategories::Opaque ? 1u : 0u;
        }
        else if (Near(md->fade, 0.5f))
        {
            // Faded: out of the prepass, whose depth would hide what shows through it.
            half += md->category == RenderCategories::Masked ? 1u : 0u;
        }
        else if (Near(md->fade, 1.0f))
        {
            gone += md->category == RenderCategories::Masked ? 1u : 0u;
        }
    }
    CHECK(solid == 1u);
    CHECK(half == 1u);
    CHECK(gone == 1u);
    CHECK(glassy == 1u);
}

TEST_CASE("extract: a skinned mesh in a scene that is not simulating reports no motion")
{
    // The animator's last two poses stay a step apart while nothing advances them; drawn as
    // motion, TAA kept dropping its history on a paused character.
    scene::Scene scene{DefaultAllocator()};
    auto* meshes = scene.AddSystem<MeshComponentManager>();
    RefPtr<geometry::StaticMesh> mesh = geometry::Primitives::Quad(DefaultAllocator());
    const Float4x4 pose[1] = {Float4x4::Translation(Float3{0.1f, 0.0f, 0.0f})};
    const Float4x4 lastPose[1] = {Float4x4::Identity()};
    MeshComponent& m = meshes->Add(scene.CreateEntity());
    m.mesh = mesh;
    m.SetMaterial(materials::MaterialBuilder(u8"skin").Shader(u8"forward").Build());
    m.boneMatrices = pose;
    m.prevBoneMatrices = lastPose;
    m.boneCount = 1;
    scene.UpdateTransforms();

    ExtractedScene running{DefaultAllocator()};
    ExtractSceneInto(scene, running);
    REQUIRE(running.Size() == 1);
    CHECK(static_cast<const MeshRenderData*>(running.Items()[0])->prevBoneMatrices == lastPose);

    scene.SetSimulationEnabled(false);
    ExtractedScene paused{DefaultAllocator()};
    ExtractSceneInto(scene, paused);
    REQUIRE(paused.Size() == 1);
    const auto* md = static_cast<const MeshRenderData*>(paused.Items()[0]);
    CHECK(md->boneMatrices == pose);         // still drawn in its pose
    CHECK(md->prevBoneMatrices == nullptr);  // and that pose as last frame's: no motion
}

TEST_CASE("instanced-mesh: seeded identity instance + entity-relative composition")
{
    scene::Scene scene(DefaultAllocator(), u8"world");
    auto* mgr = scene.AddSystem<InstancedMeshComponentManager>();
    RefPtr<geometry::StaticMesh> cube = geometry::Primitives::Cube(DefaultAllocator(), 1.0f);

    // A fresh component is seeded with ONE identity instance (editor workflow: assign a mesh,
    // see it render at the entity), and instances compose with the entity's world transform.
    scene::EntityHandle e = scene.CreateEntity(u8"scatter");
    scene.SetLocalPosition(e, Float3{5, 0, 0});
    InstancedMeshComponent& c = mgr->Add(e);
    REQUIRE(c.Count() == 1u);
    c.mesh = cube;
    scene.UpdateTransforms();

    ExtractedScene out{DefaultAllocator()};
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
    ExtractedScene out2{DefaultAllocator()};
    ExtractInstancedMeshesInto(scene, out2);
    const auto* rd2 = static_cast<const MultiMeshRenderData*>(out2.Items()[0]);
    CHECK(Near(rd2->transforms[0].m[3][1], 7.0f));
    CHECK(rd2->version != firstVersion);

    // Unmoved + unchanged: no recompose, same version (static sets stay zero-cost).
    ExtractedScene out3{DefaultAllocator()};
    ExtractInstancedMeshesInto(scene, out3);
    const auto* rd3 = static_cast<const MultiMeshRenderData*>(out3.Items()[0]);
    CHECK(rd3->version == rd2->version);

    // Authored instances are entity-relative: replace the seed with two local offsets.
    const Float4x4 xf[2] = {Float4x4::Translation(Float3{1, 0, 0}),
                            Float4x4::Translation(Float3{-1, 0, 0})};
    c.SetInstances(Span<const Float4x4>{xf, 2});
    ExtractedScene out4{DefaultAllocator()};
    ExtractInstancedMeshesInto(scene, out4);
    const auto* rd4 = static_cast<const MultiMeshRenderData*>(out4.Items()[0]);
    REQUIRE(rd4->instanceCount == 2u);
    CHECK(Near(rd4->transforms[0].m[3][0], 6.0f)); // 1 + entity x=5
    CHECK(Near(rd4->transforms[1].m[3][0], 4.0f)); // -1 + entity x=5
}

TEST_CASE("instanced-mesh: members draw after the authored instances, where they are, on their clips")
{
    scene::Scene scene(DefaultAllocator(), u8"world");
    auto* mgr = scene.AddSystem<InstancedMeshComponentManager>();
    auto* members = scene.AddSystem<InstanceMemberComponentManager>();
    RefPtr<geometry::StaticMesh> cube = geometry::Primitives::Cube(DefaultAllocator(), 1.0f);

    // A crowd: the set's own entity at x = 5 with one authored instance on clip 1, and two guests
    // moved as entities, on clips 2 and 3.
    const scene::EntityHandle crowd = scene.CreateEntity(u8"Guests");
    scene.SetLocalPosition(crowd, Float3{5, 0, 0});
    InstancedMeshComponent& set = mgr->Add(crowd);
    set.mesh = cube;
    set.instanceClips.PushBack(1);
    auto guest = [&](StringView name, Float3 at, u32 clip)
    {
        const scene::EntityHandle e = scene.CreateEntity(name);
        scene.SetLocalPosition(e, at);
        InstanceMemberComponent& m = members->Add(e);
        m.instancedMesh = scene.GetEntityId(crowd);
        m.clip = clip;
        return e;
    };
    const scene::EntityHandle a = guest(u8"A", Float3{10, 0, 0}, 2);
    const scene::EntityHandle b = guest(u8"B", Float3{0, 0, 20}, 3);
    scene.UpdateTransforms();

    ExtractedScene out{DefaultAllocator()};
    ExtractInstancedMeshesInto(scene, out);
    REQUIRE(out.Items().Size() == 1u);
    const auto* rd = static_cast<const MultiMeshRenderData*>(out.Items()[0]);
    REQUIRE(rd->instanceCount == 3u);
    CHECK(Near(rd->transforms[0].m[3][0], 5.0f));  // the authored instance, entity-relative
    CHECK(Near(rd->transforms[1].m[3][0], 10.0f)); // A at its own world position
    CHECK(Near(rd->transforms[2].m[3][2], 20.0f)); // B
    REQUIRE(rd->instanceClips != nullptr);
    CHECK(rd->instanceClips[0] == 1u);
    CHECK(rd->instanceClips[1] == 2u);
    CHECK(rd->instanceClips[2] == 3u);
    const u32 firstVersion = rd->version;

    // A member moves: its instance follows on the next extract (a new upload).
    scene.SetLocalPosition(a, Float3{12, 0, 0});
    scene.UpdateTransforms();
    ExtractedScene moved{DefaultAllocator()};
    ExtractInstancedMeshesInto(scene, moved);
    const auto* rd2 = static_cast<const MultiMeshRenderData*>(moved.Items()[0]);
    CHECK(Near(rd2->transforms[1].m[3][0], 12.0f));
    CHECK(rd2->version != firstVersion);

    // An inactive member, or one naming no instanced mesh, is not drawn.
    scene.SetActive(b, false);
    const scene::EntityHandle stray = scene.CreateEntity(u8"Stray");
    members->Add(stray).instancedMesh = Guid{9, 9};
    ExtractedScene fewer{DefaultAllocator()};
    ExtractInstancedMeshesInto(scene, fewer);
    CHECK(static_cast<const MultiMeshRenderData*>(fewer.Items()[0])->instanceCount == 2u);

    // With no authored instances the set draws its members alone.
    set.SetInstances(Span<const Float4x4>{});
    set.instanceClips.Clear();
    ExtractedScene only{DefaultAllocator()};
    ExtractInstancedMeshesInto(scene, only);
    REQUIRE(only.Items().Size() == 1u);
    const auto* rd3 = static_cast<const MultiMeshRenderData*>(only.Items()[0]);
    REQUIRE(rd3->instanceCount == 1u);
    CHECK(Near(rd3->transforms[0].m[3][0], 12.0f));
    CHECK(rd3->instanceClips[0] == 2u);
    // Members are drawn, never authored: the set's own list stays empty for a save.
    CHECK(set.instances.IsEmpty());
}

// A member's tint: its instance's colour (decoded to linear), times the set's colour when the set
// authors no tints; untinted members leave the set drawing with its one shared colour.
TEST_CASE("instanced-mesh: a member draws with its own tint, times the set's colour")
{
    scene::Scene scene(DefaultAllocator(), u8"world");
    auto* mgr = scene.AddSystem<InstancedMeshComponentManager>();
    auto* members = scene.AddSystem<InstanceMemberComponentManager>();
    RefPtr<geometry::StaticMesh> cube = geometry::Primitives::Cube(DefaultAllocator(), 1.0f);
    const scene::EntityHandle crowd = scene.CreateEntity(u8"Guests");
    InstancedMeshComponent& set = mgr->Add(crowd);
    set.mesh = cube;
    set.color = Color{0.5f, 1.0f, 1.0f, 1.0f};
    const Float4x4 authored[1] = {Float4x4::Identity()};
    set.SetInstances(Span<const Float4x4>{authored, 1});
    const scene::EntityHandle a = scene.CreateEntity(u8"A");
    members->Add(a).instancedMesh = scene.GetEntityId(crowd);
    const scene::EntityHandle b = scene.CreateEntity(u8"B");
    members->Add(b).instancedMesh = scene.GetEntityId(crowd);
    scene.UpdateTransforms();
    const auto extract = [&](ExtractedScene& out)
    {
        ExtractInstancedMeshesInto(scene, out);
        REQUIRE(out.Items().Size() == 1u);
        return static_cast<const MultiMeshRenderData*>(out.Items()[0]);
    };

    // Untinted: one shared colour, no per-instance list.
    ExtractedScene plain{DefaultAllocator()};
    CHECK(extract(plain)->tints == nullptr);

    // B glows red: every instance gets a tint, the authored one and A the set's colour, B its red
    // times it.
    members->Get(b)->tint = Color{1.0f, 0.0f, 0.0f, 1.0f};
    ExtractedScene glowing{DefaultAllocator()};
    const MultiMeshRenderData* rd = extract(glowing);
    REQUIRE(rd->instanceCount == 3u);
    REQUIRE(rd->tints != nullptr);
    const Color shared = ToLinear(set.color);
    CHECK(NearlyEqual(rd->tints[0], shared, 1.0e-4f));
    CHECK(NearlyEqual(rd->tints[1], shared, 1.0e-4f));
    CHECK(NearlyEqual(rd->tints[2], Color{shared.r, 0.0f, 0.0f, 1.0f}, 1.0e-4f));

    // With authored tints the set's colour steps aside: B is its own red, decoded.
    set.tints.PushBack(Color{0.0f, 0.0f, 1.0f, 1.0f});
    ++set.version;
    members->Get(b)->tint = Color{0.5f, 0.0f, 0.0f, 1.0f};
    ExtractedScene authoredTints{DefaultAllocator()};
    rd = extract(authoredTints);
    REQUIRE(rd->tints != nullptr);
    CHECK(NearlyEqual(rd->tints[0], Color{0.0f, 0.0f, 1.0f, 1.0f}, 1.0e-4f));
    CHECK(NearlyEqual(rd->tints[1], Color{1.0f, 1.0f, 1.0f, 1.0f}, 1.0e-4f));
    CHECK(NearlyEqual(rd->tints[2], ToLinear(Color{0.5f, 0.0f, 0.0f, 1.0f}), 1.0e-4f));
}

TEST_CASE("instanced-mesh: a member's tint is saved and read back")
{
    RegisterRenderComponentReflection();
    CHECK(TypeOf<InstanceMemberComponent>().dataVersion == 1u); // v1: tint
    String text;
    {
        foundation::xml::XmlSerializer writer(DefaultAllocator());
        InstanceMemberComponent member;
        member.clip = 2;
        member.tint = Color{1.0f, 0.5f, 0.25f, 0.4f};
        Serialize(writer, member);
        writer.GetOutput(text);
    }
    foundation::xml::XmlDocument doc(DefaultAllocator());
    REQUIRE(doc.Parse(text.AsView()) == foundation::xml::XmlResult::Ok);
    foundation::xml::XmlSerializer reader(doc);
    InstanceMemberComponent back;
    Serialize(reader, back);
    CHECK(reader.IsOk());
    CHECK(back.clip == 2u);
    CHECK(NearlyEqual(back.tint, Color{1.0f, 0.5f, 0.25f, 0.4f}, 1.0e-6f));
}

TEST_CASE("extract: authored (sRGB) colours reach render data decoded to linear")
{
    // What is entered is what is seen: every authored colour is sRGB, like an sRGB image, and
    // the renderer works in linear. Extraction is where the one decode happens.
    const Color authored{0.5f, 0.25f, 0.75f, 0.5f};
    const Color linear = ToLinear(authored);
    const auto same = [](Color a, Color b) { return NearlyEqual(a, b, 1.0e-4f); };
    const auto same3 = [](Float3 a, Color b)
    { return Near(a.x, b.r) && Near(a.y, b.g) && Near(a.z, b.b); };

    scene::Scene scene(DefaultAllocator(), u8"colours");
    auto* sets = scene.AddSystem<InstancedMeshComponentManager>();
    auto* sprites = scene.AddSystem<SpriteComponentManager>();
    auto* decals = scene.AddSystem<DecalComponentManager>();
    auto* lights = scene.AddSystem<LightComponentManager>();
    auto* cameras = scene.AddSystem<CameraComponentManager>();
    auto* env = scene.AddSystem<EnvironmentSystem>();
    RefPtr<geometry::StaticMesh> cube = geometry::Primitives::Cube(DefaultAllocator(), 1.0f);
    rhi::TextureView* fakeView = reinterpret_cast<rhi::TextureView*>(0x1); // extract only stores it

    InstancedMeshComponent& set = sets->Add(scene.CreateEntity(u8"set"));
    set.mesh = cube;
    set.color = authored;
    set.tints.PushBack(Color{0.5f, 0.5f, 0.5f, 1.0f});
    set.SetInstances(Span<const Float4x4>{&set.instances[0], 1});
    SpriteComponent& sprite = sprites->Add(scene.CreateEntity(u8"sprite"));
    sprite.texture = fakeView;
    sprite.tint = authored;
    DecalComponent& decal = decals->Add(scene.CreateEntity(u8"decal"));
    decal.texture = fakeView;
    decal.color = authored;
    LightComponent& light = lights->Add(scene.CreateEntity(u8"light"));
    light.color = authored;
    CameraComponent& cam = cameras->Add(scene.CreateEntity(u8"camera"));
    cam.clearColor = authored;
    EnvironmentSettings& e = env->Environment();
    e.ambientColor = authored;
    e.ambientIntensity = 1.0f;
    e.skyHorizon = authored;
    e.skyZenith = authored;
    e.skyGround = authored;
    scene.UpdateTransforms();

    // A separate scene for the plain mesh: ExtractSceneInto gathers several kinds of item.
    scene::Scene meshScene(DefaultAllocator(), u8"mesh");
    MeshComponent& mesh = meshScene.AddSystem<MeshComponentManager>()->Add(
        meshScene.CreateEntity(u8"mesh"));
    mesh.mesh = cube;
    mesh.color = authored;
    meshScene.UpdateTransforms();
    ExtractedScene meshOut{DefaultAllocator()};
    ExtractSceneInto(meshScene, meshOut);
    REQUIRE(meshOut.Items().Size() == 1u);
    CHECK(same(static_cast<const MeshRenderData*>(meshOut.Items()[0])->color, linear));

    ExtractedScene setOut{DefaultAllocator()};
    ExtractInstancedMeshesInto(scene, setOut);
    REQUIRE(setOut.Items().Size() == 1u);
    const auto* setData = static_cast<const MultiMeshRenderData*>(setOut.Items()[0]);
    CHECK(same(setData->color, linear));
    REQUIRE(setData->tints != nullptr);
    CHECK(Near(setData->tints[0].r, SrgbToLinear(0.5f))); // per-instance tints too
    CHECK(Near(set.tints[0].r, 0.5f));                    // the authored array is untouched

    ExtractedScene spriteOut{DefaultAllocator()};
    ExtractSpritesInto(scene, spriteOut, /*rendererId*/ 1);
    REQUIRE(spriteOut.Items().Size() == 1u);
    CHECK(same(static_cast<const SpriteRenderData*>(spriteOut.Items()[0])->tint, linear));

    ExtractedScene rest{DefaultAllocator()};
    ExtractDecalsInto(scene, rest);
    ExtractLightsInto(scene, rest);
    ExtractEnvironmentInto(scene, rest);
    REQUIRE(rest.Decals().Size() == 1u);
    CHECK(same(rest.Decals()[0].color, linear));
    REQUIRE(rest.Lights().Size() == 1u);
    CHECK(same3(rest.Lights()[0].color, linear));
    CHECK(same3(rest.Ambient(), linear));
    CHECK(same3(rest.Sky().horizon, linear));
    CHECK(same3(rest.Sky().zenith, linear));
    CHECK(same3(rest.Sky().ground, linear));

    ViewCamera vc;
    Color clear;
    REQUIRE(ExtractPrimaryCamera(scene, vc, &clear));
    CHECK(same(clear, linear));
}

TEST_CASE("ExtractEnvironmentInto carries the sky texture product (uid identity, cube flag)")
{
    foundation::scene::Scene scene(DefaultAllocator(), u8"s");
    auto* env = scene.AddSystem<engine::render::EnvironmentSystem>();
    env->Environment().skyMode = foundation::render::SkyMode::Cubemap;

    // A cube-shaped product (no GPU objects needed - identity/shape are what extraction reads).
    RefPtr<foundation::texture::Texture> sky =
        MakeRef<foundation::texture::Texture>(DefaultAllocator());
    sky->Adopt(nullptr, nullptr, nullptr, nullptr, 64, 64, rhi::TextureFormat::RGBA8Unorm,
               /*isCube*/ true);
    env->Environment().skyTexture =
        sky.Get(); // direct override (picker/serialized path binds by guid)

    // The IBL lighting dimmers ride the snapshot (defaults 1 = full physical strength).
    env->Environment().iblDiffuseIntensity = 0.4f;
    env->Environment().iblSpecularIntensity = 0.7f;

    foundation::render::ExtractedScene out{DefaultAllocator()};
    engine::render::ExtractEnvironmentInto(scene, out);
    CHECK(out.Sky().mode == foundation::render::SkyMode::Cubemap);
    CHECK(out.Sky().textureUid == sky->Uid());
    CHECK(out.Sky().textureUid != 0u);
    CHECK(out.Sky().textureIsCube);
    CHECK(out.Sky().iblDiffuseIntensity == doctest::Approx(0.4f));
    CHECK(out.Sky().iblSpecularIntensity == doctest::Approx(0.7f));

    // No texture -> no identity (the IBL keeps its programmatic/procedural source).
    env->Environment().skyTexture = foundation::resource::Ref<foundation::texture::Texture>{};
    foundation::render::ExtractedScene out2{DefaultAllocator()};
    engine::render::ExtractEnvironmentInto(scene, out2);
    CHECK(out2.Sky().textureUid == 0u);
}

TEST_CASE("extraction refreshes the material cache from the refs EVERY frame (late binds heal)")
{
    // The Sponza symptom: multi-materials that resolve AFTER the first frame (cook finishing
    // in the background) must not stay null - the cache is not a one-shot resolve-time
    // snapshot. Extraction re-reads the refs per frame.
    scene::Scene scene(DefaultAllocator(), u8"world");
    auto* meshes = scene.AddSystem<MeshComponentManager>();

    RefPtr<geometry::StaticMesh> cube = geometry::Primitives::Cube(DefaultAllocator(), 1.0f);
    RefPtr<materials::Material> matA =
        materials::MaterialBuilder(u8"a").Shader(u8"forward").Build();
    RefPtr<materials::Material> matB =
        materials::MaterialBuilder(u8"b").Shader(u8"forward").Build();

    scene::EntityHandle e = scene.CreateEntity(u8"multi");
    MeshComponent& mc = meshes->Add(e);
    mc.mesh = cube;
    // Two slots: slot 0 resolved, slot 1 UNRESOLVED (a bare guid - the pre-cook state).
    mc.materials.PushBack(foundation::resource::Ref<materials::Material>(matA));
    foundation::resource::Ref<materials::Material> late;
    late.SetId(Guid{0x1, 0x2});
    mc.materials.PushBack(late);

    ExtractedScene first{DefaultAllocator()};
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

    ExtractedScene second{DefaultAllocator()};
    ExtractSceneInto(scene, second);
    REQUIRE(second.Items().Size() == 1u);
    {
        const auto* rd = static_cast<const MeshRenderData*>(second.Items()[0]);
        CHECK(rd->submeshMaterials[1].Get() == matB.Get()); // healed - no reopen needed
    }

    // Single-entry list = whole-mesh path (no submesh routing), serving the old single-
    // material setup through the same array.
    mc.materials.Resize(1);
    ExtractedScene third{DefaultAllocator()};
    ExtractSceneInto(scene, third);
    {
        const auto* rd = static_cast<const MeshRenderData*>(third.Items()[0]);
        CHECK(rd->material == matA.Get());
        CHECK(rd->submeshMaterialCount == 0u);
    }
}

TEST_CASE("instanced-mesh: a material list draws a part per slot, and late binds heal")
{
    // An instanced mesh of several parts (a guest's body, gown and mask) carries a material per
    // submesh slot, as a mesh does: slot 0 is the primary, several slots route per submesh, and
    // the view is refreshed from the refs every frame.
    scene::Scene scene(DefaultAllocator(), u8"world");
    auto* mgr = scene.AddSystem<InstancedMeshComponentManager>();
    RefPtr<geometry::StaticMesh> cube = geometry::Primitives::Cube(DefaultAllocator(), 1.0f);
    RefPtr<materials::Material> matA = materials::MaterialBuilder(u8"a").Shader(u8"forward").Build();
    RefPtr<materials::Material> matB = materials::MaterialBuilder(u8"b").Shader(u8"forward").Build();

    InstancedMeshComponent& c = mgr->Add(scene.CreateEntity(u8"crowd"));
    c.mesh = cube;
    c.materials.PushBack(foundation::resource::Ref<materials::Material>(matA));
    foundation::resource::Ref<materials::Material> late;
    late.SetId(Guid{0x1, 0x2});
    c.materials.PushBack(late);
    scene.UpdateTransforms();

    ExtractedScene first{DefaultAllocator()};
    ExtractInstancedMeshesInto(scene, first);
    REQUIRE(first.Items().Size() == 1u);
    {
        const auto* rd = static_cast<const MultiMeshRenderData*>(first.Items()[0]);
        CHECK(rd->material == matA.Get());
        REQUIRE(rd->submeshMaterialCount == 2u);
        CHECK(rd->submeshMaterials[1].Get() == nullptr); // not cooked yet
    }

    c.materials[1].SetDirect(RefPtr<materials::Material>(matB.Get()));
    ExtractedScene second{DefaultAllocator()};
    ExtractInstancedMeshesInto(scene, second);
    CHECK(static_cast<const MultiMeshRenderData*>(second.Items()[0])->submeshMaterials[1].Get() == matB.Get());

    // One material covers the whole mesh: no submesh routing.
    c.SetMaterial(matB);
    ExtractedScene third{DefaultAllocator()};
    ExtractInstancedMeshesInto(scene, third);
    {
        const auto* rd = static_cast<const MultiMeshRenderData*>(third.Items()[0]);
        CHECK(rd->material == matB.Get());
        CHECK(rd->submeshMaterialCount == 0u);
    }
}

TEST_CASE("extract: postTonemap sprites land in the WorldUI category (authored colors)")
{
    scene::Scene scene{DefaultAllocator(), u8"world"};
    scene.AddSystem<SpriteComponentManager>();
    const scene::EntityHandle e = scene.CreateEntity(u8"panel");
    SpriteComponent& sprite = scene.GetSystem<SpriteComponentManager>()->Add(e);
    rhi::TextureView* fakeView = reinterpret_cast<rhi::TextureView*>(0x1); // extract only stores it
    sprite.texture = fakeView;
    sprite.postTonemap = true;
    sprite.orientation = SpriteOrientation::EntityOriented;
    scene.UpdateTransforms();

    ExtractedScene out{DefaultAllocator()};
    ExtractSpritesInto(scene, out, /*rendererId*/ 1);
    REQUIRE(out.Items().Size() == 1u);
    const auto* rd = static_cast<const SpriteRenderData*>(out.Items()[0]);
    CHECK(rd->category == RenderCategories::WorldUI);
    CHECK(rd->postTonemap);
    CHECK(Categories().Affinity(rd->category) == PassAffinity::PostTonemap);
    CHECK(Categories().Sort(rd->category) == SortMode::BackToFront);

    // The default path stays in Transparent.
    sprite.postTonemap = false;
    ExtractedScene plain{DefaultAllocator()};
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
    scene::Scene a(DefaultAllocator(), u8"look");
    PostProcessSystem* postA = a.AddSystem<PostProcessSystem>();
    postA->Post().exposureEV = 1.5f;
    postA->Post().tonemapOperator = TonemapOperator::Clamp;
    postA->Post().bloomIntensity = 0.2f;
    postA->Post().aoMode = AoMode::GTAO;
    postA->Post().ssrEnabled = true;
    postA->Post().ssgiEnabled = true;
    postA->Post().ssgiIntensity = 1.5f;
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
    scene::Scene b{DefaultAllocator()};
    PostProcessSystem* postB = b.AddSystem<PostProcessSystem>();
    {
        BinarySerializer reader(stream, SerializeMode::Read);
        SerializeScene(reader, b);
        REQUIRE(reader.IsOk());
    }
    const PostProcessSettings& r = postB->Post();
    CHECK(Near(r.exposureEV, 1.5f));
    CHECK(r.tonemapOperator == TonemapOperator::Clamp);
    CHECK(Near(r.bloomIntensity, 0.2f));
    CHECK(r.aoMode == AoMode::GTAO);
    CHECK(r.ssrEnabled);
    CHECK(r.ssgiEnabled);           // v3 fields ride the versioned payload
    CHECK(Near(r.ssgiIntensity, 1.5f));
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
    // SSGI: enable + intensity map straight through.
    {
        PostProcessSettings gi;
        gi.ssgiEnabled = true;
        gi.ssgiIntensity = 2.0f;
        const ViewPostConfig vp = ResolveScenePost(gi);
        CHECK(vp.ssgiEnabled);
        CHECK(Near(vp.ssgiIntensity, 2.0f));
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
        vp.ssgiEnabled = true;
        vp.taaEnabled = true;
        vp.fxaaEnabled = false;
        vp.exposure = 2.0f;
        vp.autoExposure = true;
        return vp;
    };

    // A single flag strips just its effect.
    {
        ViewPostConfig vp = base();
        ApplyViewPostOverride(vp, ViewPostOverride{.disableBloom = true});
        CHECK_FALSE(vp.bloomEnabled);
        CHECK(vp.aoMode == 1u);
        CHECK(vp.autoExposure); // per-effect flags leave adaptation alone
    }
    {
        // The master toggle also FREEZES auto exposure (editing clarity: the viewport must not
        // shift brightness with the camera); the fixed authored EV survives.
        ViewPostConfig vp = base();
        ApplyViewPostOverride(vp, ViewPostOverride{.disablePost = true});
        CHECK_FALSE(vp.autoExposure);
        CHECK_FALSE(vp.ssgiEnabled); // the master toggle drops the GI bounce too
        CHECK(vp.exposure == doctest::Approx(2.0f));
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
    // type must expose BOTH (authored intent, not name heuristics).
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
    foundation::vfs::NativeFileSystem dataFs(foundation::vfs::FindDataRoot(), DefaultAllocator());
    RenderSubsystem sub{DefaultAllocator(), device, 2, dataFs};
    // View-frustum culling is ON by default for every host of the subsystem (editor, player,
    // samples) since 2026-09-24; a per-view ViewPostOverride::disableCulling is the A/B.
    CHECK(sub.ViewCulling());

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

    // AND the per-scene list is a third, independent buffer: a keyed view draws BOTH its own
    // list and the scene's (2026-08-18 fix: the original either/or made keyed views - the edit
    // viewport - silently drop ALL scene-level debug draw: physics + navmesh invisible in the
    // editor while PIE showed them). Isolation still holds: scene list != any view list.
    scene::Scene worldScene(DefaultAllocator(), u8"debug-scene");
    debug::DebugDraw& sceneDbg = sub.DebugScene(worldScene);
    CHECK(&sceneDbg != &mainDbg);
    CHECK(&sceneDbg != &previewDbg);
    sceneDbg.DrawLine(Float3{0, 0, 0}, Float3{0, 1, 0}, Color{1, 1, 1, 1});
    CHECK(sceneDbg.HasAnyDraws());
    CHECK_FALSE(previewDbg.HasAnyDraws()); // scene draws never mutate a view list
}

TEST_CASE("render: a view carries the scene AND view debug lists independently (keyed-view fix)")
{
    // The RenderView plumbing behind the fix: AddView hands the frame BOTH lists; the debug
    // pass merges global + scene + view per view. Pin the storage contract headlessly.
    RenderView view;
    int sceneList = 0;
    int viewList = 0;
    view.SetDebugScene(&sceneList);
    view.SetDebugView(&viewList);
    CHECK(view.DebugScene() == &sceneList);
    CHECK(view.DebugViewList() == &viewList);
    view.SetDebugView(nullptr); // unkeyed views carry no view list
    CHECK(view.DebugScene() == &sceneList);
    CHECK(view.DebugViewList() == nullptr);
}

TEST_CASE("extract: effectively-inactive entities render NOTHING; toggling restores exactly")
{
    // One gate per extraction loop, driven by the effective-active cache - so an inactive
    // PARENT hides a child's renderables without touching own flags.
    scene::Scene scene(DefaultAllocator(), u8"active-gate");
    auto* meshes = scene.AddSystem<MeshComponentManager>();
    auto* instanced = scene.AddSystem<InstancedMeshComponentManager>();
    auto* sprites = scene.AddSystem<SpriteComponentManager>();
    auto* lights = scene.AddSystem<LightComponentManager>();
    auto* probes = scene.AddSystem<ReflectionProbeComponentManager>();

    RefPtr<geometry::StaticMesh> mesh = geometry::Primitives::Cube(DefaultAllocator(), 1.0f);

    scene::EntityHandle parent = scene.CreateEntity(u8"parent");
    meshes->Add(parent).mesh = mesh;

    scene::EntityHandle child = scene.CreateEntity(u8"child");
    scene.SetParent(child, parent);
    {
        InstancedMeshComponent& c = instanced->Add(child);
        c.mesh = mesh;
        c.instances.PushBack(Float4x4::Identity());
        ++c.version;
    }
    lights->Add(child);
    probes->Add(child);

    scene::EntityHandle bystander = scene.CreateEntity(u8"bystander");
    meshes->Add(bystander).mesh = mesh;

    scene.UpdateTransforms();

    const auto extractAll = [&](ExtractedScene& out)
    {
        ExtractSceneInto(scene, out);
        ExtractInstancedMeshesInto(scene, out);
        ExtractSpritesInto(scene, out, /*spriteRendererId=*/1);
        ExtractLightsInto(scene, out);
        ExtractReflectionProbesInto(scene, out);
    };

    {
        ExtractedScene all{DefaultAllocator()};
        extractAll(all);
        CHECK(all.Size() == 3);                        // parent mesh + child instanced + bystander
        CHECK(all.Lights().Size() == 1);
        CHECK(all.ReflectionProbes().Size() == 1);
    }

    // Deactivate the PARENT: the whole subtree goes dark; own flags below are untouched.
    scene.SetActive(parent, false);
    {
        ExtractedScene dark{DefaultAllocator()};
        extractAll(dark);
        CHECK(dark.Size() == 1); // only the bystander survives
        CHECK(dark.Lights().Size() == 0);
        CHECK(dark.ReflectionProbes().Size() == 0);
        CHECK(scene.IsActive(child));
    }

    // Reactivate: everything returns.
    scene.SetActive(parent, true);
    {
        ExtractedScene restored{DefaultAllocator()};
        extractAll(restored);
        CHECK(restored.Size() == 3);
        CHECK(restored.Lights().Size() == 1);
        CHECK(restored.ReflectionProbes().Size() == 1);
    }
    (void)sprites;
}

TEST_CASE("extract: a mesh's own material properties reach its draw, with their version")
{
    scene::Scene scene(DefaultAllocator(), u8"overrides");
    auto* meshes = scene.AddSystem<MeshComponentManager>();
    RefPtr<geometry::StaticMesh> cube = geometry::Primitives::Cube(DefaultAllocator(), 1.0f);
    const scene::EntityHandle plain = scene.CreateEntity(u8"plain");
    meshes->Add(plain).mesh = cube;
    const scene::EntityHandle glowing = scene.CreateEntity(u8"glowing");
    MeshComponent& mc = meshes->Add(glowing);
    mc.mesh = cube;
    mc.SetMaterialProperty(0, u8"EmissiveColor", Float4{0.3f, 0.4f, 0.5f, 2.0f}, sizeof(Float4));
    mc.SetMaterialProperty(0, u8"Roughness", Float4{0.2f, 0, 0, 0}, sizeof(f32));
    scene.UpdateTransforms();

    const auto drawOf = [&](ExtractedScene& out, scene::EntityHandle e) -> const MeshRenderData*
    {
        for (RenderData* d : out.Items())
        {
            if (d != nullptr && d->entityId == PackEntity(e))
            {
                return static_cast<const MeshRenderData*>(d);
            }
        }
        return nullptr;
    };
    ExtractedScene out{DefaultAllocator()};
    ExtractSceneInto(scene, out);
    const MeshRenderData* a = drawOf(out, plain);
    const MeshRenderData* b = drawOf(out, glowing);
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    CHECK(a->overrideCount == 0u);
    CHECK(a->overrides == nullptr);
    REQUIRE(b->overrideCount == 2u);
    CHECK(b->overrides[0].name == StringView(u8"EmissiveColor"));
    CHECK(b->overrides[0].value.w == doctest::Approx(2.0f));
    CHECK(b->overrides[1].size == sizeof(f32));
    const u32 version = b->overrideVersion;

    // A change (here a clear) bumps the version the renderer compares.
    CHECK(mc.ClearMaterialProperty(0, u8"Roughness"));
    CHECK_FALSE(mc.ClearMaterialProperty(0, u8"Roughness"));
    ExtractedScene later{DefaultAllocator()};
    ExtractSceneInto(scene, later);
    const MeshRenderData* c = drawOf(later, glowing);
    REQUIRE(c != nullptr);
    CHECK(c->overrideCount == 1u);
    CHECK(c->overrideVersion != version);
}

TEST_CASE("extract: a light's shadow controls reach the shadow it casts")
{
    scene::Scene scene(DefaultAllocator(), u8"shadows");
    auto* lights = scene.AddSystem<LightComponentManager>();
    LightComponent& sun = lights->Add(scene.CreateEntity(u8"sun"));
    sun.castsShadows = true;
    sun.shadowNormalBias = 1.5f;
    sun.shadowDepthBiasScale = 2.0f;
    sun.shadowStrength = 0.4f;
    LightComponent& spot = lights->Add(scene.CreateEntity(u8"spot"));
    spot.type = LightType::Spot;
    spot.castsShadows = true;
    spot.shadowNormalBias = 0.5f;
    spot.shadowDepthBiasScale = 3.0f;
    scene.UpdateTransforms();

    ExtractedScene out{DefaultAllocator()};
    ExtractLightsInto(scene, out);
    const DirectionalShadow& ds = out.DirectionalShadowData();
    REQUIRE(ds.valid);
    CHECK(Near(ds.normalBias, 1.5f));
    CHECK(Near(ds.depthBias, ShadowBiasDefaults::kDepthBias * 2.0f)); // a scale of the default
    CHECK(Near(ds.strength, 0.4f));
    REQUIRE(out.Lights().Size() == 2u);
    CHECK(Near(out.Lights()[0].shadowStrength, 0.4f)); // the forward shader's per-light lerp
    CHECK(Near(out.Lights()[1].shadowStrength, 1.0f)); // the default: a full shadow
    REQUIRE(out.LocalShadowCasters().Size() == 1u);
    CHECK(Near(out.LocalShadowCasters()[0].normalBias, 0.5f));
    CHECK(Near(out.LocalShadowCasters()[0].depthBias, ShadowBiasDefaults::kLocalDepthBias * 3.0f));

    // A built atlas entry carries them: the depth bias as is, the normal offset as world units per
    // unit of distance (its texels times the tile's texel size at distance 1).
    const GpuLocalShadow entry = BuildSpotShadow(out.LocalShadowCasters()[0], 0, 2048, 512);
    CHECK(Near(entry.depthBias, ShadowBiasDefaults::kLocalDepthBias * 3.0f));
    const f32 fov = Min(spot.outerAngle * 2.0f + 0.05f, 3.0f);
    CHECK(Abs(entry.normalBiasPerDistance - 0.5f * 2.0f * Tan(fov * 0.5f) / 512.0f) < 1e-6f);
}

TEST_CASE("extract: the local shadow tiles go to the lights nearest the view")
{
    // Five shadowed point lights in a row, 10 m apart; a point takes six of a layer's sixteen tiles,
    // so two fit in the realtime layer.
    scene::Scene scene(DefaultAllocator(), u8"tiles");
    auto* lights = scene.AddSystem<LightComponentManager>();
    for (u32 i = 0; i < 5; ++i)
    {
        scene::EntityHandle e = scene.CreateEntity(u8"torch");
        scene.SetLocalPosition(e, Float3{10.0f * static_cast<f32>(i), 0.0f, 0.0f});
        LightComponent& lc = lights->Add(e);
        lc.type = LightType::Point;
        lc.range = 4.0f;
        lc.castsShadows = true;
    }
    scene.UpdateTransforms();
    const auto shadowed = [](const ExtractedScene& out)
    {
        Array<u32> which(DefaultAllocator());
        for (u32 i = 0; i < out.Lights().Size(); ++i)
        {
            if (out.Lights()[i].shadowIndex >= 0.0f)
            {
                which.PushBack(i);
            }
        }
        return which;
    };

    SUBCASE("without a view, the first that fit")
    {
        ExtractedScene out{DefaultAllocator()};
        ExtractLightsInto(scene, out);
        const Array<u32> which = shadowed(out);
        REQUIRE(which.Size() == 2u);
        CHECK(which[0] == 0u);
        CHECK(which[1] == 1u);
    }
    SUBCASE("with a view, the nearest; their tiles in the order the lights came")
    {
        ExtractedScene out{DefaultAllocator()};
        out.SetViewOrigin(Float3{39.0f, 3.0f, 0.0f}); // inside the last one's reach, near the fourth
        ExtractLightsInto(scene, out);
        const Array<u32> which = shadowed(out);
        REQUIRE(which.Size() == 2u);
        CHECK(which[0] == 3u);
        CHECK(which[1] == 4u);
        CHECK(out.Lights()[3].shadowIndex == 0.0f); // the earlier light takes the first entries
        CHECK(out.Lights()[4].shadowIndex == 6.0f);
        REQUIRE(out.LocalShadowCasters().Size() == 2u);
        CHECK(Near(out.LocalShadowCasters()[0].positionWS.x, 30.0f));
        CHECK(Near(out.LocalShadowCasters()[1].positionWS.x, 40.0f));

        // Walking past them to the far side changes which is nearer, not where their tiles are.
        ExtractedScene later{DefaultAllocator()};
        later.SetViewOrigin(Float3{50.0f, 3.0f, 0.0f});
        ExtractLightsInto(scene, later);
        CHECK(later.Lights()[3].shadowIndex == 0.0f);
        CHECK(later.Lights()[4].shadowIndex == 6.0f);
    }
    SUBCASE("a spot still fits where a point no longer does")
    {
        scene::EntityHandle e = scene.CreateEntity(u8"lantern");
        scene.SetLocalPosition(e, Float3{100.0f, 0.0f, 0.0f});
        LightComponent& spot = lights->Add(e);
        spot.type = LightType::Spot;
        spot.castsShadows = true;
        scene.UpdateTransforms();
        ExtractedScene out{DefaultAllocator()};
        out.SetViewOrigin(Float3{0.0f, 3.0f, 0.0f});
        ExtractLightsInto(scene, out);
        const Array<u32> which = shadowed(out);
        REQUIRE(which.Size() == 3u);
        CHECK(which[2] == 5u);
        CHECK(out.Lights()[5].shadowIndex == 12.0f);
    }
    SUBCASE("the static layer has a budget of its own")
    {
        u32 n = 0;
        lights->ForEach(
            [&](LightComponent& lc, scene::EntityHandle)
            {
                if (n++ >= 2)
                {
                    lc.shadowUpdate = ShadowUpdateMode::Static;
                }
            });
        ExtractedScene out{DefaultAllocator()};
        ExtractLightsInto(scene, out);
        const Array<u32> which = shadowed(out);
        REQUIRE(which.Size() == 4u); // two realtime, two of the three static
        CHECK(which[2] == 2u);
        CHECK(which[3] == 3u);
        CHECK(out.LocalShadowCasters()[2].isStatic);
    }
}

TEST_CASE("shadows: a cached tile redraws while a moving caster is in it and once after it leaves")
{
    const Float3 torch{0.0f, 1.5f, 0.0f};
    const Float4 inside[] = {Float4{2.0f, 0.9f, 0.0f, 1.0f}};
    const Float4 away[] = {Float4{20.0f, 0.9f, 0.0f, 1.0f}};
    const Float4 grazing[] = {Float4{6.5f, 0.9f, 0.0f, 1.0f}}; // its sphere reaches the 6 m volume
    CHECK(MovingCasterTouchesTile(torch, 6.0f, Span<const Float4>{inside}, {}));
    CHECK(MovingCasterTouchesTile(torch, 6.0f, Span<const Float4>{grazing}, {}));
    CHECK_FALSE(MovingCasterTouchesTile(torch, 6.0f, Span<const Float4>{away}, {}));
    // Walked out this frame: last frame's sphere still redraws the tile, so his shadow leaves too.
    CHECK(MovingCasterTouchesTile(torch, 6.0f, Span<const Float4>{away}, Span<const Float4>{inside}));
    CHECK_FALSE(MovingCasterTouchesTile(torch, 6.0f, Span<const Float4>{away}, Span<const Float4>{away}));
}

TEST_CASE("light: the shadow controls round-trip with the scene (v1)")
{
    scene::Scene a{DefaultAllocator()};
    LightComponent& written = a.AddSystem<LightComponentManager>()->Add(a.CreateEntity(u8"sun"));
    written.shadowNormalBias = 0.75f;
    written.shadowDepthBiasScale = 1.5f;
    written.shadowStrength = 0.25f;

    MemoryStream stream;
    {
        BinarySerializer writer(stream, SerializeMode::Write);
        SerializeScene(writer, a);
        REQUIRE(writer.IsOk());
    }
    (void)stream.Seek(0, SeekOrigin::Begin);
    scene::Scene b{DefaultAllocator()};
    LightComponentManager* lightsB = b.AddSystem<LightComponentManager>();
    {
        BinarySerializer reader(stream, SerializeMode::Read);
        SerializeScene(reader, b);
        REQUIRE(reader.IsOk());
    }
    const LightComponent* read = nullptr;
    lightsB->ForEach([&](LightComponent& c, scene::EntityHandle) { read = &c; });
    REQUIRE(read != nullptr);
    CHECK(Near(read->shadowNormalBias, 0.75f));
    CHECK(Near(read->shadowDepthBiasScale, 1.5f));
    CHECK(Near(read->shadowStrength, 0.25f));
    CHECK(TypeOf<LightComponent>().dataVersion == 1u);
}

TEST_CASE("camera: MakeCameraProjection builds a perspective or an orthographic matrix")
{
    CameraComponent cam;
    cam.fovYRadians = 1.2f;
    cam.nearZ = 0.5f;
    cam.farZ = 300.0f;
    cam.orthoHeight = 40.0f;

    const Float4x4 perspective = MakeCameraProjection(cam, 2.0f);
    const Float4x4 expectedPerspective = Float4x4::PerspectiveFovRH(1.2f, 2.0f, 0.5f, 300.0f);
    cam.projection = CameraProjection::Orthographic;
    const Float4x4 orthographic = MakeCameraProjection(cam, 2.0f);
    // The height is authored; the width follows the aspect.
    const Float4x4 expectedOrthographic = Float4x4::OrthographicRH(80.0f, 40.0f, 0.5f, 300.0f);
    for (usize r = 0; r < 4; ++r)
    {
        for (usize c = 0; c < 4; ++c)
        {
            CHECK(Near(perspective(r, c), expectedPerspective(r, c)));
            CHECK(Near(orthographic(r, c), expectedOrthographic(r, c)));
        }
    }
    CHECK_FALSE(projection::IsOrthographic(perspective));
    CHECK(projection::IsOrthographic(orthographic));
}

TEST_CASE("extract: an orthographic primary camera renders orthographic at the view's aspect")
{
    scene::Scene scene(DefaultAllocator(), u8"cam-ortho");
    auto* cameras = scene.AddSystem<CameraComponentManager>();
    const scene::EntityHandle e = scene.CreateEntity(u8"top");
    CameraComponent& cam = cameras->Add(e);
    cam.projection = CameraProjection::Orthographic;
    cam.orthoHeight = 20.0f;
    scene.UpdateTransforms();

    ViewCamera vc;
    REQUIRE(ExtractPrimaryCamera(scene, vc, nullptr, 1.5f));
    CHECK(projection::IsOrthographic(vc.projection));
    CHECK(Near(vc.projection(0, 0), 2.0f / 30.0f)); // width = 20 x 1.5
    CHECK(Near(vc.projection(1, 1), 2.0f / 20.0f));
}

TEST_CASE("camera: the projection mode and the orthographic height round-trip with the scene")
{
    scene::Scene a{DefaultAllocator()};
    CameraComponentManager* camerasA = a.AddSystem<CameraComponentManager>();
    CameraComponent& written = camerasA->Add(a.CreateEntity(u8"map"));
    written.projection = CameraProjection::Orthographic;
    written.orthoHeight = 64.0f;

    MemoryStream stream;
    {
        BinarySerializer writer(stream, SerializeMode::Write);
        SerializeScene(writer, a);
        REQUIRE(writer.IsOk());
    }
    (void)stream.Seek(0, SeekOrigin::Begin);
    scene::Scene b{DefaultAllocator()};
    CameraComponentManager* camerasB = b.AddSystem<CameraComponentManager>();
    {
        BinarySerializer reader(stream, SerializeMode::Read);
        SerializeScene(reader, b);
        REQUIRE(reader.IsOk());
    }
    const CameraComponent* read = nullptr;
    camerasB->ForEach([&](CameraComponent& c, scene::EntityHandle) { read = &c; });
    REQUIRE(read != nullptr);
    CHECK(read->projection == CameraProjection::Orthographic);
    CHECK(Near(read->orthoHeight, 64.0f));
}

namespace
{
    // A live render-texture product on the null device, the way the factory makes one.
    RefPtr<foundation::texture::Texture> MakeTargetTexture(rhi::Device& device, u32 width,
                                                           u32 height)
    {
        rhi::Texture* gpu = nullptr;
        REQUIRE(device
                    .CreateTexture(rhi::TextureDesc::RenderTarget(
                                       rhi::TextureFormat::RGBA8UnormSrgb, width, height),
                                   gpu)
                    .IsOk());
        rhi::TextureView* view = nullptr;
        REQUIRE(device.CreateTextureView(gpu, rhi::TextureViewDesc{}, view).IsOk());
        RefPtr<foundation::texture::Texture> texture =
            MakeRef<foundation::texture::Texture>(DefaultAllocator());
        texture->Adopt(&device, gpu, view, nullptr, width, height,
                       rhi::TextureFormat::RGBA8UnormSrgb);
        return texture;
    }
}

TEST_CASE("extract: a camera with a target is never the screen camera, primary or not")
{
    rhi::null::NullDevice device{DefaultAllocator()};
    scene::Scene scene(DefaultAllocator(), u8"cam-target-pick");
    auto* cameras = scene.AddSystem<CameraComponentManager>();
    const scene::EntityHandle minimap = scene.CreateEntity(u8"minimap");
    scene.SetLocalPosition(minimap, Float3{0, 50, 0});
    cameras->Add(minimap).target = MakeTargetTexture(device, 64, 64); // primary by default
    const scene::EntityHandle player = scene.CreateEntity(u8"player");
    scene.SetLocalPosition(player, Float3{0, 2, 0});
    cameras->Add(player);
    scene.UpdateTransforms();

    ViewCamera vc;
    REQUIRE(ExtractPrimaryCamera(scene, vc));
    CHECK(Near(vc.position.y, 2.0f)); // the player's camera, though the minimap comes first

    // An asset target that has not loaded still marks the camera as a target camera.
    CameraComponent unloaded;
    unloaded.target.SetId(Guid{0x1234, 0x5678});
    CHECK(unloaded.HasTarget());
    CHECK_FALSE(CameraComponent{}.HasTarget());
}

TEST_CASE("extract: target cameras render at their texture's aspect, active and on their interval")
{
    rhi::null::NullDevice device{DefaultAllocator()};
    scene::Scene scene(DefaultAllocator(), u8"cam-targets");
    auto* cameras = scene.AddSystem<CameraComponentManager>();

    const scene::EntityHandle map = scene.CreateEntity(u8"map");
    scene.SetLocalPosition(map, Float3{0, 40, 0});
    CameraComponent& mapCam = cameras->Add(map);
    mapCam.projection = CameraProjection::Orthographic;
    mapCam.orthoHeight = 30.0f;
    mapCam.clearColor = Color{0.0f, 0.5f, 0.0f, 1.0f};
    mapCam.target = MakeTargetTexture(device, 200, 100);

    const scene::EntityHandle monitor = scene.CreateEntity(u8"monitor");
    CameraComponent& monitorCam = cameras->Add(monitor);
    monitorCam.target = MakeTargetTexture(device, 64, 64);
    monitorCam.targetInterval = 3;

    const scene::EntityHandle screen = scene.CreateEntity(u8"screen");
    cameras->Add(screen); // no target: the screen's camera, never collected
    scene.UpdateTransforms();

    Array<TargetCameraView> views;
    CollectTargetCameras(scene, 3, views); // frame 3: both are due
    REQUIRE(views.Size() == 2u);
    CHECK(views[0].target == mapCam.target.Get()); // manager order, stable frame to frame
    CHECK(views[1].target == monitorCam.target.Get());
    // The map renders orthographic at its texture's 2:1 aspect, from where its entity is.
    const Float4x4 expected = Float4x4::OrthographicRH(60.0f, 30.0f, mapCam.nearZ, mapCam.farZ);
    for (usize r = 0; r < 4; ++r)
    {
        for (usize c = 0; c < 4; ++c)
        {
            CHECK(Near(views[0].camera.camera.projection(r, c), expected(r, c)));
        }
    }
    CHECK(Near(views[0].camera.camera.position.y, 40.0f));
    CHECK(Near(views[0].camera.clearColor.g, SrgbToLinear(0.5f))); // authored sRGB, decoded

    CollectTargetCameras(scene, 4, views); // the monitor draws every third frame only
    REQUIRE(views.Size() == 1u);
    CHECK(views[0].target == mapCam.target.Get());

    scene.SetActive(map, false);
    CollectTargetCameras(scene, 6, views);
    REQUIRE(views.Size() == 1u);
    CHECK(views[0].target == monitorCam.target.Get());

    // A target whose texture is gone (a failed load) is skipped, not drawn into.
    monitorCam.target = RefPtr<foundation::texture::Texture>{};
    CollectTargetCameras(scene, 6, views);
    CHECK(views.IsEmpty());
}

TEST_CASE("camera: the target and its interval round-trip with the scene")
{
    const Guid texture{0xABCD, 0x1234};
    scene::Scene a{DefaultAllocator()};
    CameraComponentManager* camerasA = a.AddSystem<CameraComponentManager>();
    CameraComponent& written = camerasA->Add(a.CreateEntity(u8"monitor"));
    written.target.SetId(texture);
    written.targetInterval = 2;

    MemoryStream stream;
    {
        BinarySerializer writer(stream, SerializeMode::Write);
        SerializeScene(writer, a);
        REQUIRE(writer.IsOk());
    }
    (void)stream.Seek(0, SeekOrigin::Begin);
    scene::Scene b{DefaultAllocator()};
    CameraComponentManager* camerasB = b.AddSystem<CameraComponentManager>();
    {
        BinarySerializer reader(stream, SerializeMode::Read);
        SerializeScene(reader, b);
        REQUIRE(reader.IsOk());
    }
    const CameraComponent* read = nullptr;
    camerasB->ForEach([&](CameraComponent& c, scene::EntityHandle) { read = &c; });
    REQUIRE(read != nullptr);
    CHECK(read->target.id == texture);
    CHECK(read->targetInterval == 2u);
}

TEST_CASE("extract: an inactive primary camera falls through to the next primary")
{
    scene::Scene scene(DefaultAllocator(), u8"cam-fallthrough");
    auto* cameras = scene.AddSystem<CameraComponentManager>();

    scene::EntityHandle first = scene.CreateEntity(u8"first");
    scene.SetLocalPosition(first, Float3{0, 0, 5});
    cameras->Add(first).aspect = 1.0f; // primary by default

    scene::EntityHandle second = scene.CreateEntity(u8"second");
    scene.SetLocalPosition(second, Float3{0, 0, 9});
    cameras->Add(second).aspect = 1.0f;

    scene.UpdateTransforms();

    ViewCamera vc;
    REQUIRE(ExtractPrimaryCamera(scene, vc));
    CHECK(Near(vc.position.z, 5.0f)); // manager order: first wins while active

    scene.SetActive(first, false);
    REQUIRE(ExtractPrimaryCamera(scene, vc)); // falls through to the second
    CHECK(Near(vc.position.z, 9.0f));

    scene.SetActive(second, false);
    CHECK_FALSE(ExtractPrimaryCamera(scene, vc)); // no active primary at all
}

TEST_CASE("ResolveScenePost: auto-exposure window resolves EV stops to linear clamps")
{
    // Defaults: off, and no grading (a default block stays byte-identical to the old config).
    {
        PostProcessSettings d;
        const ViewPostConfig vp = ResolveScenePost(d);
        CHECK_FALSE(vp.autoExposure);
        CHECK(vp.gradingLut == nullptr);
        CHECK(Near(vp.gradingLutSize, 0.0f));
    }
    // The clamp window is authored in relative EV; the config carries linear multipliers.
    {
        PostProcessSettings s;
        s.autoExposure = true;
        s.autoExposureKey = 0.25f;
        s.autoExposureSpeed = 3.0f;
        s.autoExposureMinEV = -2.0f;
        s.autoExposureMaxEV = 3.0f;
        const ViewPostConfig vp = ResolveScenePost(s);
        CHECK(vp.autoExposure);
        CHECK(Near(vp.autoExposureKey, 0.25f));
        CHECK(Near(vp.autoExposureSpeed, 3.0f));
        CHECK(Near(vp.autoExposureMin, 0.25f)); // 2^-2
        CHECK(Near(vp.autoExposureMax, 8.0f));  // 2^3
    }
    // A grading Ref with no resolved product (or no GPU view) resolves to no grading - the
    // strip-shape guard (width == height^2) is only consulted on a live product.
    {
        PostProcessSettings s;
        s.gradingLut.SetId(Guid{0x1, 0x2}); // unbound: Get() == nullptr headlessly
        s.gradingIntensity = 0.5f;
        const ViewPostConfig vp = ResolveScenePost(s);
        CHECK(vp.gradingLut == nullptr);
        CHECK(Near(vp.gradingLutSize, 0.0f));
    }
}

TEST_CASE("EnvironmentSettings: the IBL lighting dimmers serialize with the scene (v4)")
{
    // Mirror of the PostProcessSettings round-trip: author the v4 fields, serialize the
    // whole scene, reload - the dimmers survive (and default to 1 = full strength).
    {
        engine::render::EnvironmentSettings d;
        CHECK(d.iblDiffuseIntensity == doctest::Approx(1.0f));
        CHECK(d.iblSpecularIntensity == doctest::Approx(1.0f));
    }
    foundation::scene::Scene a(DefaultAllocator(), u8"env");
    auto* envA = a.AddSystem<engine::render::EnvironmentSystem>();
    envA->Environment().iblDiffuseIntensity = 0.35f;
    envA->Environment().iblSpecularIntensity = 0.8f;
    (void)a.CreateEntity(u8"e");

    MemoryStream stream;
    {
        BinarySerializer writer(stream, SerializeMode::Write);
        SerializeScene(writer, a);
        REQUIRE(writer.IsOk());
    }
    (void)stream.Seek(0, SeekOrigin::Begin);
    foundation::scene::Scene b{DefaultAllocator()};
    auto* envB = b.AddSystem<engine::render::EnvironmentSystem>();
    {
        BinarySerializer reader(stream, SerializeMode::Read);
        SerializeScene(reader, b);
        REQUIRE(reader.IsOk());
    }
    CHECK(envB->Environment().iblDiffuseIntensity == doctest::Approx(0.35f));
    CHECK(envB->Environment().iblSpecularIntensity == doctest::Approx(0.8f));
}

TEST_CASE("EnvironmentSettings: the shadow reach serializes, and a v4 scene reads its defaults")
{
    engine::render::RegisterRenderComponentReflection();
    const TypeInfo& type = TypeOf<engine::render::EnvironmentSettings>();
    CHECK(type.dataVersion >= 5u); // v5 added the reach

    // Round trip at v5.
    engine::render::EnvironmentSystem written;
    written.Environment().shadowDistance = 70.0f;
    written.Environment().shadowCascadeSplit = 0.8f;
    written.Environment().shadowFadeDistance = 12.0f;
    written.Environment().iblSpecularIntensity = 0.6f;
    MemoryStream stream;
    {
        BinarySerializer ar(stream, SerializeMode::Write);
        BeginVersionedPayload(ar, type);
        written.SerializeSettings(ar);
        EndVersionedPayload(ar);
        REQUIRE(ar.IsOk());
    }
    (void)stream.Seek(0, SeekOrigin::Begin);
    engine::render::EnvironmentSystem read;
    {
        BinarySerializer ar(stream, SerializeMode::Read);
        BeginVersionedPayload(ar, type);
        read.SerializeSettings(ar);
        EndVersionedPayload(ar);
        REQUIRE(ar.IsOk());
    }
    CHECK(read.Environment().shadowDistance == doctest::Approx(70.0f));
    CHECK(read.Environment().shadowCascadeSplit == doctest::Approx(0.8f));
    CHECK(read.Environment().shadowFadeDistance == doctest::Approx(12.0f));

    // A v4 payload, as the stored scenes held it: the chain stamped 4 and the value fields
    // without the reach.
    MemoryStream v4;
    {
        BinarySerializer ar(v4, SerializeMode::Write);
        const SerializedDataVersion chain[] = {{type.id, 4u}};
        u32 n = 1;
        ar.Key("dataVersions");
        ar.BeginArray(n);
        SerializedDataVersion entry = chain[0];
        ar.Key("type");
        ar.Scalar(&entry.typeId, ScalarKind::UInt64);
        ar.Key("version");
        ar.Scalar(&entry.version, ScalarKind::UInt32);
        ar.EndArray();
        ar.PushVersionScope(chain, 1);
        engine::render::SerializeEnvironmentValues(ar, written.Environment(), /*shadowReach*/ false);
        ar.PopVersionScope();
        REQUIRE(ar.IsOk());
    }
    (void)v4.Seek(0, SeekOrigin::Begin);
    engine::render::EnvironmentSystem legacy;
    legacy.Environment().shadowDistance = 1.0f; // overwritten only if the reader reads the field
    {
        BinarySerializer ar(v4, SerializeMode::Read);
        BeginVersionedPayload(ar, type);
        legacy.SerializeSettings(ar);
        EndVersionedPayload(ar);
        REQUIRE(ar.IsOk());
    }
    CHECK(legacy.Environment().shadowDistance == doctest::Approx(1.0f)); // not read: v4 has none
    CHECK(legacy.Environment().iblSpecularIntensity == doctest::Approx(0.6f)); // the v4 fields read
}

TEST_CASE("extract: the scene's shadow reach rides the snapshot")
{
    foundation::scene::Scene scene(DefaultAllocator(), u8"reach");
    auto* env = scene.AddSystem<engine::render::EnvironmentSystem>();
    env->Environment().shadowDistance = 60.0f;
    env->Environment().shadowCascadeSplit = 0.7f;
    env->Environment().shadowFadeDistance = 8.0f;
    ExtractedScene out{DefaultAllocator()};
    ExtractEnvironmentInto(scene, out);
    CHECK(out.ShadowSettings().distance == doctest::Approx(60.0f));
    CHECK(out.ShadowSettings().cascadeSplit == doctest::Approx(0.7f));
    CHECK(out.ShadowSettings().fadeDistance == doctest::Approx(8.0f));

    // A shorter reach gives the near cascade smaller texels: what sharpens a roof's shadow.
    ViewCamera cam;
    cam.view = Float4x4::LookAtRH(Float3{0, 2, 0}, Float3{0, 2, -10}, Float3{0, 1, 0});
    cam.projection = Float4x4::PerspectiveFovRH(1.0472f, 16.0f / 9.0f, 0.1f, 400.0f);
    cam.farZ = 400.0f;
    const Float3 sun = Normalized(Float3{-0.17f, -0.87f, -0.47f});
    const ShadowCascades wide = ComputeCascades(cam, sun, 300.0f, 1024);
    const ShadowCascades near = ComputeCascades(cam, sun, 60.0f, 1024);
    CHECK(near.texelWorldSize[0] < wide.texelWorldSize[0] * 0.5f);
    // ...and a split nearer 1 gives the camera's surroundings more of the map.
    const ShadowCascades logarithmic = ComputeCascades(cam, sun, 60.0f, 1024, 1.0f);
    CHECK(logarithmic.texelWorldSize[0] < near.texelWorldSize[0]);
}

TEST_CASE("render: a view's camera reads its kind, planes and height from its projection")
{
    ViewCamera cam;
    cam.projection = Float4x4::PerspectiveFovRH(1.0472f, 16.0f / 9.0f, 0.5f, 300.0f);
    CHECK_FALSE(cam.IsOrthographic());
    CHECK(cam.NearZ() == doctest::Approx(0.5f).epsilon(1.0e-4));
    CHECK(cam.FarZ() == doctest::Approx(300.0f).epsilon(1.0e-4));
    CHECK(cam.OrthoHeight() == 0.0f);

    cam.projection = Float4x4::OrthographicRH(32.0f, 18.0f, 0.1f, 2000.0f);
    CHECK(cam.IsOrthographic());
    CHECK(cam.NearZ() == doctest::Approx(0.1f).epsilon(1.0e-3));
    CHECK(cam.FarZ() == doctest::Approx(2000.0f).epsilon(1.0e-4));
    CHECK(cam.OrthoHeight() == doctest::Approx(18.0f));
}

TEST_CASE("render: the shadow cascades start at the camera's own near plane")
{
    // A camera whose near plane is 2 m: with uniform splits (lambda 0) over a 60 m reach the first
    // cascade ends a quarter of the way from 2 m to 60 m, not from the 0.1 m once assumed.
    ViewCamera cam;
    cam.view = Float4x4::LookAtRH(Float3{0, 2, 0}, Float3{0, 2, -10}, Float3{0, 1, 0});
    cam.projection = Float4x4::PerspectiveFovRH(1.0472f, 16.0f / 9.0f, 2.0f, 400.0f);
    cam.farZ = 400.0f;
    const Float3 sun = Normalized(Float3{-0.17f, -0.87f, -0.47f});
    const ShadowCascades cascades = ComputeCascades(cam, sun, 60.0f, 1024, 0.0f);
    CHECK(cascades.splitFar[0] == doctest::Approx(2.0f + 58.0f / 4.0f).epsilon(1.0e-3));
}

TEST_CASE("render: an orthographic view's cascades cover its box from the near plane to the reach")
{
    // An orthographic camera 30 m back, 20 m tall: a point anywhere across the view, at any depth up
    // to the reach, lands inside the cascade that covers its depth.
    ViewCamera cam;
    const Float3 eye{0, 5, 30};
    cam.view = Float4x4::LookAtRH(eye, Float3{0, 0, 0}, Float3{0, 1, 0});
    cam.projection = Float4x4::OrthographicRH(20.0f * 16.0f / 9.0f, 20.0f, 0.1f, 200.0f);
    cam.farZ = 200.0f;
    const Float3 sun = Normalized(Float3{-0.17f, -0.87f, -0.47f});
    const ShadowCascades cascades = ComputeCascades(cam, sun, 60.0f, 1024);
    REQUIRE(cascades.valid);
    const Float4x4 toWorld = Inverse(cam.view);
    const f32 halfW = 10.0f * 16.0f / 9.0f * 0.95f, halfH = 10.0f * 0.95f;
    const f32 depths[] = {0.5f, 12.0f, 35.0f, 59.0f};
    const f32 corners[][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}, {0, 0}};
    for (const f32 depth : depths)
    {
        u32 c = 0;
        while (c + 1 < ShadowCascades::kCount && cascades.splitFar[c] < depth)
        {
            ++c;
        }
        for (const auto& k : corners)
        {
            const Float3 world = TransformPoint(Float3{k[0] * halfW, k[1] * halfH, -depth}, toWorld);
            const Float3 lightClip = TransformPoint(world, cascades.viewProj[c]);
            CAPTURE(depth);
            CAPTURE(c);
            CHECK(Abs(lightClip.x) <= 1.0f);
            CHECK(Abs(lightClip.y) <= 1.0f);
            CHECK(lightClip.z >= 0.0f);
            CHECK(lightClip.z <= 1.0f);
        }
    }
}

TEST_CASE("render: an orthographic view's sky draws through a perspective stand-in")
{
    ViewCamera cam;
    const Float3 eye{0, 2, 10};
    cam.position = eye;
    cam.view = Float4x4::LookAtRH(eye, Float3{0, 2, 0}, Float3{0, 1, 0});
    cam.projection = Float4x4::PerspectiveFovRH(1.0f, 16.0f / 9.0f, 0.1f, 500.0f);
    // A perspective view's sky is the view's own.
    const Float4x4 perspectiveSky = SkyViewProjection(cam);
    const Float4x4 perspectiveView = cam.ViewProjection();
    for (u32 r = 0; r < 4; ++r)
    {
        for (u32 c = 0; c < 4; ++c)
        {
            CHECK(perspectiveSky.m[r][c] == perspectiveView.m[r][c]);
        }
    }

    // An orthographic view's: a 60 degree perspective looking where the view looks.
    cam.projection = Float4x4::OrthographicRH(16.0f, 9.0f, 0.1f, 500.0f);
    const Float4x4 sky = SkyViewProjection(cam);
    // w carries depth again: the view-projection's w column is not zero (view.hlsli's test).
    CHECK(Abs(sky.m[0][3]) + Abs(sky.m[1][3]) + Abs(sky.m[2][3]) > 0.5f);
    const Float4x4 invSky = Inverse(sky);
    const auto ray = [&](f32 x, f32 y)
    {
        const Float4 h = Float4{x, y, projection::kNdcDepthFar, 1.0f} * invSky;
        return Normalized(Float3{h.x / h.w, h.y / h.w, h.z / h.w} - eye);
    };
    const Float3 centre = ray(0.0f, 0.0f);
    CHECK(centre.z == doctest::Approx(-1.0f).epsilon(1.0e-4));
    const Float3 top = ray(0.0f, 1.0f); // half of 60 degrees above the view
    CHECK(Atan2(top.y, -top.z) == doctest::Approx(0.5235988f).epsilon(1.0e-3));

    // A direction lands where it did for a still camera, and moves when the camera turns: the
    // sky's motion (a direction through an orthographic matrix has w = 0, no motion at all).
    const auto ndcOf = [](const Float4x4& vp, Float3 dir)
    {
        const Float4 clip = Float4{dir.x, dir.y, dir.z, 0.0f} * vp;
        return Float2{clip.x / clip.w, clip.y / clip.w};
    };
    const Float3 dir = Normalized(Float3{0.2f, 0.1f, -1.0f});
    const Float2 still = ndcOf(sky, dir);
    CHECK(still.x == doctest::Approx(ndcOf(SkyViewProjection(cam), dir).x));
    ViewCamera turned = cam;
    turned.view = Float4x4::LookAtRH(eye, Float3{0.3f, 2, 0}, Float3{0, 1, 0});
    const Float2 moved = ndcOf(SkyViewProjection(turned), dir);
    CHECK(Abs(moved.x - still.x) > 0.01f);
    CHECK(moved.x == moved.x); // finite
}

TEST_CASE("render: RequestPick keys on the viewport, answers nothing without a key, cancels")
{
    rhi::null::NullDevice device{DefaultAllocator()};
    foundation::vfs::NativeFileSystem dataFs(foundation::vfs::FindDataRoot(), DefaultAllocator());
    RenderSubsystem sub{DefaultAllocator(), device, 2, dataFs};

    int viewportKey = 0;
    // No key = no view could ever answer: refused up front (0), never pending.
    CHECK(sub.RequestPick(nullptr, 5, 5) == kInvalidPickRequest);
    CHECK_FALSE(sub.IsPickPending(kInvalidPickRequest));

    const PickRequestId id = sub.RequestPick(&viewportKey, 5, 5);
    REQUIRE(id != kInvalidPickRequest);
    CHECK(sub.IsPickPending(id));
    PickResult result;
    CHECK_FALSE(sub.TryTakePickResult(id, result)); // nothing rendered yet

    // Distinct requests get distinct ids; a rect request is accepted as-is (clamped at render).
    const PickRequestId rect = sub.RequestPick(&viewportKey, -3, -3, 40, 40);
    CHECK(rect != id);
    CHECK(sub.IsPickPending(rect));

    // The viewport goes away: its requests vanish (never answered, never pending).
    sub.CancelPicks(&viewportKey);
    CHECK_FALSE(sub.IsPickPending(id));
    CHECK_FALSE(sub.IsPickPending(rect));
    CHECK_FALSE(sub.TryTakePickResult(id, result));
}

TEST_CASE("EnvironmentSystem keeps the scene's render clock from the scene's own dt and stamps it on the snapshot")
{
    scene::Scene scene{DefaultAllocator()};
    engine::render::AddRenderSceneManagers(scene);
    auto* env = scene.GetSystem<engine::render::EnvironmentSystem>();
    REQUIRE(env != nullptr);
    scene.Start();
    CHECK(env->TimeSeconds() == 0.0f);

    // The scene's dt is what the scene manager composed (context x group x scene scales): the
    // clock adds exactly that, once per frame, keeping last frame's value for motion vectors.
    scene.Update(0.5f);
    CHECK(env->TimeSeconds() == 0.5f);
    CHECK(env->PrevTimeSeconds() == 0.0f);
    scene.Update(0.25f);
    CHECK(env->TimeSeconds() == 0.75f);
    CHECK(env->PrevTimeSeconds() == 0.5f);
    // A paused scene (scale 0 -> dt 0) holds still.
    scene.Update(0.0f);
    CHECK(env->TimeSeconds() == 0.75f);
    CHECK(env->PrevTimeSeconds() == 0.75f);
    // The editor's editing scene ticks with simulation DISABLED (Simulate enables it): the
    // clock does not advance there, so a frozen world's grass stands still.
    scene.SetSimulationEnabled(false);
    scene.Update(0.5f);
    CHECK(env->TimeSeconds() == 0.75f);
    CHECK(env->PrevTimeSeconds() == 0.75f);
    scene.SetSimulationEnabled(true);
    scene.Update(0.5f);
    CHECK(env->TimeSeconds() == 1.25f);
    scene.SetSimulationEnabled(false); // the extraction below expects 1.25 / 0.75
    scene.Update(0.5f);

    // Extraction stamps the clock on the snapshot; a snapshot of a scene without the
    // environment system carries none (the frame clock stands in).
    ExtractedScene snapshot{DefaultAllocator()};
    engine::render::ExtractEnvironmentInto(scene, snapshot);
    CHECK(snapshot.HasTime());
    CHECK(snapshot.TimeSeconds() == 1.25f);
    CHECK(snapshot.PrevTimeSeconds() == 0.75f);
    scene::Scene bare{DefaultAllocator()};
    ExtractedScene none{DefaultAllocator()};
    engine::render::ExtractEnvironmentInto(bare, none);
    CHECK(!none.HasTime());
}

TEST_CASE("EnvironmentSettings: the default colours are sRGB and render as they always have")
{
    // The defaults were picked while colours were read raw; written in sRGB, they decode to the
    // same linear values, so a new scene looks as it did.
    const EnvironmentSettings e;
    const auto decodesTo = [](Color c, f32 r, f32 g, f32 b)
    {
        const Color l = ToLinear(c);
        return Abs(l.r - r) < 2e-3f && Abs(l.g - g) < 2e-3f && Abs(l.b - b) < 2e-3f;
    };
    CHECK(decodesTo(e.ambientColor, 0.10f, 0.12f, 0.16f));
    CHECK(decodesTo(e.skyHorizon, 0.52f, 0.60f, 0.70f));
    CHECK(decodesTo(e.skyZenith, 0.20f, 0.36f, 0.58f));
    CHECK(decodesTo(e.skyGround, 0.26f, 0.26f, 0.26f));
}

TEST_CASE("entity bounds: a mesh measures through its world matrix, an instanced set by its sphere")
{
    scene::Scene scene(DefaultAllocator(), u8"world");
    auto* meshes = scene.AddSystem<MeshComponentManager>();
    auto* sets = scene.AddSystem<InstancedMeshComponentManager>();
    RefPtr<geometry::StaticMesh> cube = geometry::Primitives::Cube(DefaultAllocator(), 1.0f);
    const AABB local = cube->bounds;
    REQUIRE(local.IsValid());

    // A mesh scaled 2 and moved: its bounds through that matrix.
    const scene::EntityHandle crate = scene.CreateEntity(u8"crate");
    Transform t;
    t.position = Float3{3.0f, 0.0f, 0.0f};
    t.scale = Float3{2.0f, 2.0f, 2.0f};
    scene.SetLocalTransform(crate, t);
    MeshComponent& m = meshes->Add(crate);
    scene.UpdateTransforms();
    AABB out;
    CHECK_FALSE(scene::EntityWorldBounds(scene, crate, out)); // no mesh assigned yet
    m.mesh = cube;
    REQUIRE(scene::EntityWorldBounds(scene, crate, out));
    CHECK(Near(out.min.x, 3.0f + 2.0f * local.min.x));
    CHECK(Near(out.max.x, 3.0f + 2.0f * local.max.x));
    CHECK(Near(out.max.y, 2.0f * local.max.y));

    // An instanced set: nothing until extraction has measured it, then its merged sphere.
    const scene::EntityHandle scatter = scene.CreateEntity(u8"scatter");
    scene.SetLocalPosition(scatter, Float3{-5.0f, 0.0f, 0.0f});
    InstancedMeshComponent& c = sets->Add(scatter);
    c.mesh = cube;
    scene.UpdateTransforms();
    CHECK_FALSE(scene::EntityWorldBounds(scene, scatter, out));
    ExtractedScene extracted{DefaultAllocator()};
    ExtractInstancedMeshesInto(scene, extracted);
    REQUIRE(scene::EntityWorldBounds(scene, scatter, out));
    CHECK(Near(out.Center().x, -5.0f));
    CHECK(out.Extents().x > 0.0f);

    // An entity with neither.
    CHECK_FALSE(scene::EntityWorldBounds(scene, scene.CreateEntity(u8"empty"), out));
}

TEST_CASE("render: the debug lists empty at every iteration's end, a frame rendered or not")
{
    // A minimized window renders no frame (the host skips BeginRendering/EndRendering) while the
    // app keeps drawing: the lists must not grow across those iterations.
    rhi::null::NullDevice device{DefaultAllocator()};
    foundation::vfs::NativeFileSystem dataFs(foundation::vfs::FindDataRoot(), DefaultAllocator());
    RenderSubsystem sub{DefaultAllocator(), device, 2, dataFs};
    scene::Scene world(DefaultAllocator(), u8"world");
    int viewKey = 0;
    for (int iteration = 0; iteration < 100; ++iteration)
    {
        sub.DebugGlobal().DrawLine(Float3{0, 0, 0}, Float3{1, 0, 0}, Color{1, 1, 1, 1});
        sub.DebugScene(world).DrawLine(Float3{0, 0, 0}, Float3{0, 1, 0}, Color{1, 1, 1, 1});
        sub.DebugView(&viewKey).DrawLine(Float3{0, 0, 0}, Float3{0, 0, 1}, Color{1, 1, 1, 1});
        sub.DebugScreen().DrawScreenRect(0, 0, 10, 10, Color{1, 1, 1, 1});
        CHECK(sub.DebugGlobal().LineVertices().Size() == 2u); // this iteration's line alone
        sub.EndFrame(); // no frame rendered: the end of the iteration still drains them
    }
    CHECK_FALSE(sub.DebugGlobal().HasAnyDraws());
    CHECK_FALSE(sub.DebugScene(world).HasAnyDraws());
    CHECK_FALSE(sub.DebugView(&viewKey).HasAnyDraws());
    CHECK_FALSE(sub.DebugScreen().HasAnyDraws());
}

// The inspector offers an enum's REGISTERED values only: EntityOriented, the one orientation that
// turns with its entity, was missing from the reflection, so no authored sprite could use it.
TEST_CASE("components: every sprite orientation is offered to the editor")
{
    RegisterRenderComponentReflection();
    const TypeInfo& type = TypeOf<SpriteOrientation>();
    CHECK(EnumeratorCount(type) == 4u);
    const char* name = EnumValueName(type, static_cast<i64>(SpriteOrientation::EntityOriented));
    REQUIRE(name != nullptr);
    CHECK(StringView(reinterpret_cast<const utf8char*>(name)) == StringView(u8"EntityOriented"));
}

// A script asks a probe to capture with ReflectionProbeComponent.of(entity).capture(); the request
// count rides the extracted probe to the probe system, which recaptures on a count it has not seen.
TEST_CASE("extract: a reflection probe's capture requests reach the extracted probe")
{
    scene::Scene scene(DefaultAllocator(), u8"probe-capture");
    auto* probes = scene.AddSystem<ReflectionProbeComponentManager>();
    scene::EntityHandle e = scene.CreateEntity(u8"probe");
    ReflectionProbeComponent& probe = probes->Add(e);
    probe.update = ProbeUpdateMode::Manual;
    scene.UpdateTransforms();

    ExtractedScene before{DefaultAllocator()};
    ExtractReflectionProbesInto(scene, before);
    REQUIRE(before.ReflectionProbes().Size() == 1u);
    probe.capture();
    ExtractedScene after{DefaultAllocator()};
    ExtractReflectionProbesInto(scene, after);
    REQUIRE(after.ReflectionProbes().Size() == 1u);
    CHECK(after.ReflectionProbes()[0].captureRequest != before.ReflectionProbes()[0].captureRequest);
}

// The camera depth prepass holds only what its forward draw will place on exactly the same depth:
// an opaque material whose shader moves its vertices with a depth program of its own (a surface
// shader's Vertex()) is drawn without it, as a fading mesh is; masked and blended keep theirs.
TEST_CASE("extract: a material with its own depth program stays out of the depth prepass")
{
    RefPtr<materials::Material> plain = materials::CreatePBR(u8"plain");
    CHECK(CategoryForMaterial(plain.Get()) == RenderCategories::Opaque);
    RefPtr<materials::Material> waves = materials::CreatePBR(u8"waves");
    waves->depthShaderName = String(u8"waves.depth");
    CHECK(CategoryForMaterial(waves.Get()) == RenderCategories::Masked);
    waves->pipeline.blendMode = materials::BlendMode::AlphaBlend;
    CHECK(CategoryForMaterial(waves.Get()) == RenderCategories::Transparent);
    CHECK(CategoryForMaterial(nullptr) == RenderCategories::Opaque);
}

namespace
{
    // An overlay that records the view each pass hands it.
    struct RecordingOverlay final : foundation::render::IScreenOverlay
    {
        void Render(rhi::RenderPassEncoder&, const foundation::render::ScreenOverlayView& view) override
        {
            last = view;
            ++calls;
        }
        foundation::render::ScreenOverlayView last;
        int calls = 0;
    };
}

// A host with a UNORM view of its sRGB target hands it to the overlay pass: the overlays are told
// the target is encoded, and its format is the view's (VG then blends encoded values). Without
// one, the target as it is.
TEST_CASE("render: the screen overlay pass draws through the target's UNORM view when given one")
{
    rhi::null::NullDevice device{DefaultAllocator()};
    foundation::vfs::NativeFileSystem dataFs(foundation::vfs::FindDataRoot(), DefaultAllocator());
    RenderSubsystem sub{DefaultAllocator(), device, 2, dataFs};
    RecordingOverlay overlay;
    sub.RegisterOverlay(&overlay);

    rhi::TextureDesc td = rhi::TextureDesc::RenderTarget(rhi::TextureFormat::RGBA8UnormSrgb, 8, 8);
    td.alternateViewFormat = rhi::TextureFormat::RGBA8Unorm;
    rhi::Texture* target = nullptr;
    REQUIRE(device.CreateTexture(td, target).IsOk());
    rhi::TextureView* view = nullptr;
    REQUIRE(device.CreateTextureView(target, rhi::TextureViewDesc{}, view).IsOk());
    rhi::TextureViewDesc unormDesc{};
    unormDesc.format = rhi::TextureFormat::RGBA8Unorm;
    rhi::TextureView* unorm = nullptr;
    REQUIRE(device.CreateTextureView(target, unormDesc, unorm).IsOk());
    rhi::CommandPool* pool = nullptr;
    REQUIRE(device.CreateCommandPool(rhi::QueueType::Graphics, pool).IsOk());
    rhi::CommandEncoder* encoder = nullptr;
    REQUIRE(pool->CreateEncoder(encoder).IsOk());

    sub.RenderOverlays(*encoder, view, rhi::TextureFormat::RGBA8UnormSrgb, 8, 8, 0);
    REQUIRE(overlay.calls == 1);
    CHECK_FALSE(overlay.last.encodedTarget);
    CHECK(overlay.last.targetFormat == rhi::TextureFormat::RGBA8UnormSrgb);

    sub.RenderOverlays(*encoder, view, rhi::TextureFormat::RGBA8UnormSrgb, 8, 8, 0, unorm);
    REQUIRE(overlay.calls == 2);
    CHECK(overlay.last.encodedTarget);
    CHECK(overlay.last.targetFormat == rhi::TextureFormat::RGBA8Unorm);

    sub.UnregisterOverlay(&overlay);
    pool->DestroyEncoder(encoder);
    device.DestroyCommandPool(pool);
    device.DestroyTextureView(unorm);
    device.DestroyTextureView(view);
    device.DestroyTexture(target);
}

