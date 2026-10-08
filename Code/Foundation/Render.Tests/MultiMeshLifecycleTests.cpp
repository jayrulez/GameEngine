// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// MultiMesh set lifecycle + the per-item shadow opt-out, the two renderer contracts
// vegetation stands on: a set unseen for kMultiMeshEvictFrames leaves the renderer's pool
// (its buffers and bind groups through the retire queue when one is wired), and a RenderData
// with castShadows = false is absent from the shadow caster list.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

#include <initializer_list>

import foundation.core;
import foundation.vfs;
import foundation.rhi;
import foundation.rhi.null;
import foundation.geometry;
import foundation.materials;
import foundation.materials.pipelinecache;
import foundation.shaders;
import foundation.shaders.system;
import foundation.render;

using namespace foundation::core;
using namespace foundation::render;
namespace rhi = foundation::rhi;
namespace geometry = foundation::geometry;
namespace materials = foundation::materials;
namespace shaders = foundation::shaders;

namespace
{
    struct Systems
    {
        shaders::Compiler* compiler = nullptr;
        rhi::null::NullDevice device{DefaultAllocator()};
        bool Init()
        {
            return shaders::createCompiler(shaders::CompilerDesc{}, compiler).IsOk();
        }
        ~Systems()
        {
            if (compiler != nullptr)
            {
                compiler->Destroy();
            }
        }
    };

    void AddSet(ExtractedScene& scene, u64 key, geometry::StaticMesh* mesh,
                materials::Material* material, Span<const Float4x4> transforms, u16 rendererId,
                u32 version = 1, u32 uploadCount = 0)
    {
        MultiMeshRenderData* rd = scene.Add<MultiMeshRenderData>();
        REQUIRE(rd != nullptr);
        rd->multiMesh = true;
        rd->key = key;
        rd->transforms = transforms.Data();
        rd->instanceCount = static_cast<u32>(transforms.Size());
        rd->uploadCount = uploadCount;
        rd->version = version;
        rd->mesh = mesh;
        rd->material = material;
        rd->rendererId = rendererId;
        rd->category = RenderCategories::Opaque;
        rd->worldRadius = 2.0f;
    }
}

TEST_CASE("multimesh: a set unseen for kMultiMeshEvictFrames leaves the pool, retired when a queue is wired")
{
    Systems s;
    if (!s.Init())
    {
        MESSAGE("no shader compiler; skipping");
        return;
    }
    shaders::ShaderSystem shaderSystem(*s.compiler, s.device);
    materials::PipelineStateCache psoCache(shaderSystem, s.device);
    materials::MaterialSystem materialSystem;
    REQUIRE(materialSystem.Initialize(s.device).IsOk());
    MeshRenderer renderer(DefaultAllocator(), s.device, shaderSystem, psoCache, materialSystem, /*framesInFlight*/ 2);
    REQUIRE(renderer.Initialize().IsOk());
    GpuRetireQueue retire;
    retire.Initialize(&s.device, 2);
    renderer.SetRetireQueue(&retire);

    RefPtr<geometry::StaticMesh> cube = geometry::Primitives::Cube(DefaultAllocator(), 1.0f);
    RefPtr<materials::Material> material =
        materials::MaterialBuilder(u8"lit").Shader(u8"forward").Build();
    Array<Float4x4> transforms;
    transforms.Resize(3, Float4x4::Identity());

    ExtractedScene seen{DefaultAllocator()};
    AddSet(seen, 0x11u, cube.Get(), material.Get(), Span<const Float4x4>{transforms.Data(), 3},
           renderer.RendererId());
    AddSet(seen, 0x22u, cube.Get(), material.Get(), Span<const Float4x4>{transforms.Data(), 3},
           renderer.RendererId());
    renderer.PrepareFrame(2, 0);
    renderer.UploadMultiMeshes(seen);
    CHECK(renderer.MultiMeshSetCount() == 2u);
    const usize pendingAfterUpload = retire.PendingCount();

    // Set 0x22 keeps being extracted; 0x11 vanishes (its chunk left the camera's range).
    ExtractedScene partial{DefaultAllocator()};
    AddSet(partial, 0x22u, cube.Get(), material.Get(), Span<const Float4x4>{transforms.Data(), 3},
           renderer.RendererId());
    for (u32 f = 0; f < MeshRenderer::kMultiMeshEvictFrames; ++f)
    {
        renderer.PrepareFrame(2, f % 2);
        renderer.UploadMultiMeshes(partial);
        CHECK(renderer.MultiMeshSetCount() == 2u); // not yet: exactly the window
    }
    renderer.PrepareFrame(2, 0);
    renderer.UploadMultiMeshes(partial); // one past the window
    CHECK(renderer.MultiMeshSetCount() == 1u);
    // Its buffer + the two region bind groups went through the queue, not vkDestroy in place.
    CHECK(retire.PendingCount() >= pendingAfterUpload + 3u);

    // An empty frame still ages: the survivor goes too once unseen long enough.
    ExtractedScene empty{DefaultAllocator()};
    for (u32 f = 0; f <= MeshRenderer::kMultiMeshEvictFrames; ++f)
    {
        renderer.PrepareFrame(2, f % 2);
        renderer.UploadMultiMeshes(empty);
    }
    CHECK(renderer.MultiMeshSetCount() == 0u);

    // A returning key rebuilds a fresh set.
    renderer.PrepareFrame(2, 0);
    renderer.UploadMultiMeshes(seen);
    CHECK(renderer.MultiMeshSetCount() == 2u);
    retire.Flush();
    renderer.SetRetireQueue(nullptr);
}

TEST_CASE("shadows: castShadows = false keeps an Opaque item out of the caster list")
{
    Systems s;
    if (!s.Init())
    {
        MESSAGE("no shader compiler; skipping");
        return;
    }
    shaders::ShaderSystem shaderSystem(*s.compiler, s.device);
    materials::PipelineStateCache psoCache(shaderSystem, s.device);
    materials::MaterialSystem materialSystem;
    REQUIRE(materialSystem.Initialize(s.device).IsOk());
    MeshRenderer renderer(DefaultAllocator(), s.device, shaderSystem, psoCache, materialSystem, 2);
    REQUIRE(renderer.Initialize().IsOk());
    RendererRegistry registry;
    registry.Register(&renderer);
    ShadowSystem shadows(s.device, 2);
    REQUIRE(shadows.Initialize().IsOk());
    RenderFrame frame(DefaultAllocator(), s.device, registry, 2, nullptr, nullptr, &shadows);

    RefPtr<geometry::StaticMesh> cube = geometry::Primitives::Cube(DefaultAllocator(), 1.0f);
    RefPtr<materials::Material> material =
        materials::MaterialBuilder(u8"lit").Shader(u8"forward").Build();
    ExtractedScene scene{DefaultAllocator()};
    for (int i = 0; i < 3; ++i)
    {
        MeshRenderData* rd = scene.Add<MeshRenderData>();
        rd->mesh = cube.Get();
        rd->material = material.Get();
        rd->category = RenderCategories::Opaque;
        rd->rendererId = renderer.RendererId();
        rd->worldCenter = Float3{static_cast<f32>(i) * 3.0f, 0, 0};
        rd->worldRadius = 1.0f;
        rd->castShadows = (i != 1); // the middle one is a filler that casts nothing
    }
    DirectionalShadow ds;
    ds.direction = Normalized(Float3{0.3f, -1.0f, 0.2f});
    ds.valid = true;
    scene.SetDirectionalShadow(ds);

    rhi::CommandPool* pool = nullptr;
    REQUIRE(s.device.CreateCommandPool(rhi::QueueType::Graphics, pool).IsOk());
    rhi::CommandEncoder* encoder = nullptr;
    REQUIRE(pool->CreateEncoder(encoder).IsOk());
    rhi::Texture* color = nullptr;
    REQUIRE(s.device
                .CreateTexture(rhi::TextureDesc::RenderTarget(rhi::TextureFormat::BGRA8Unorm, 64, 64),
                               color)
                .IsOk());
    rhi::TextureView* colorView = nullptr;
    REQUIRE(s.device.CreateTextureView(color, rhi::TextureViewDesc{}, colorView).IsOk());

    ViewCamera camera;
    camera.view = Float4x4::LookAtRH(Float3{0, 0, 10}, Float3{0, 0, 0}, Float3{0, 1, 0});
    camera.projection = Float4x4::PerspectiveFovRH(1.0472f, 1.0f, 0.1f, 100.0f);
    ViewSettings settings;
    frame.Begin(*encoder, 0);
    frame.AddView(scene, camera, settings, colorView, rhi::TextureFormat::BGRA8Unorm, 64, 64);
    frame.End();
    CHECK(frame.ShadowCasterCount(&scene) == 2u);

    (void)encoder->Finish();
    s.device.WaitIdle();
    s.device.DestroyTextureView(colorView);
    s.device.DestroyTexture(color);
    pool->DestroyEncoder(encoder);
    s.device.DestroyCommandPool(pool);
}

TEST_CASE("multimesh: a set's region capacity keeps every region offset storage-aligned on every backend")
{
    // 144-byte InstanceData x a multiple of 16 = a multiple of 2304 = 9 x 256: the strictest
    // storage-buffer offset alignment (WebGPU + D3D12) holds for region 1, 2, ... at any count.
    for (const u32 count : {1u, 15u, 16u, 17u, 941u, 1024u, 2048u, 4095u, 4096u})
    {
        const u32 capacity = MeshRenderer::MultiMeshRegionCapacity(count);
        INFO(count);
        CHECK(capacity >= count);
        CHECK(capacity < count + 16u);
        CHECK(capacity % 16u == 0u);
        CHECK((static_cast<u64>(capacity) * 144u) % 256u == 0u);
    }
    CHECK(MeshRenderer::MultiMeshRegionCapacity(0) == 0u);
}

TEST_CASE("multimesh: a draw count that grows within the capacity rewrites the region it outgrew")
{
    // The vegetation fade draws a PREFIX of a set's instances that changes with the camera's
    // distance, under one unchanged version. A region written with a short prefix and then drawn
    // with a longer one showed stale bytes for the tail - per region, so the tail blinked between
    // frames-in-flight (the loaded-scene prop flicker of 2026-09-22).
    Systems s;
    if (!s.Init())
    {
        MESSAGE("no shader compiler; skipping");
        return;
    }
    shaders::ShaderSystem shaderSystem(*s.compiler, s.device);
    materials::PipelineStateCache psoCache(shaderSystem, s.device);
    materials::MaterialSystem materialSystem;
    REQUIRE(materialSystem.Initialize(s.device).IsOk());
    MeshRenderer renderer(DefaultAllocator(), s.device, shaderSystem, psoCache, materialSystem, /*framesInFlight*/ 2);
    REQUIRE(renderer.Initialize().IsOk());

    RefPtr<geometry::StaticMesh> cube = geometry::Primitives::Cube(DefaultAllocator(), 1.0f);
    RefPtr<materials::Material> material =
        materials::MaterialBuilder(u8"lit").Shader(u8"forward").Build();
    Array<Float4x4> transforms; // eight distinct translations, one set
    for (u32 i = 0; i < 8; ++i)
    {
        transforms.PushBack(Float4x4::Translation(Float3{static_cast<f32>(i), 0.0f, 0.0f}));
    }
    auto frame = [&](u32 index, u32 count, u32 version)
    {
        ExtractedScene scene{DefaultAllocator()};
        AddSet(scene, 0x77u, cube.Get(), material.Get(),
               Span<const Float4x4>{transforms.Data(), count}, renderer.RendererId(), version);
        renderer.PrepareFrame(2, index % 2);
        renderer.UploadMultiMeshes(scene);
    };
    Float4x4 world;

    // Frame 0 draws three (far away), frame 1 all eight (region 1 holds them all)...
    frame(0, 3, 1);
    frame(1, 8, 1);
    REQUIRE(renderer.ReadMultiMeshInstance(0x77u, 1, 7, world));
    CHECK(world.m[3][0] == doctest::Approx(7.0f));
    // ...and frame 2 draws eight from region 0, which only ever held three: it is rewritten.
    frame(2, 8, 1);
    REQUIRE(renderer.ReadMultiMeshInstance(0x77u, 0, 7, world));
    CHECK(world.m[3][0] == doctest::Approx(7.0f));

    // A new version rewrites each region on its next frame, even at the same count.
    for (Float4x4& m : transforms)
    {
        m.m[3][2] = 5.0f;
    }
    frame(3, 8, 2);
    frame(4, 8, 2);
    REQUIRE(renderer.ReadMultiMeshInstance(0x77u, 0, 0, world));
    CHECK(world.m[3][2] == doctest::Approx(5.0f));
    REQUIRE(renderer.ReadMultiMeshInstance(0x77u, 1, 0, world));
    CHECK(world.m[3][2] == doctest::Approx(5.0f));

    // A shrink under the same version writes nothing: the regions already hold the longer prefix.
    transforms[7].m[3][1] = 9.0f; // a change the renderer was NOT told about (no version bump)
    frame(5, 4, 2);
    frame(6, 4, 2);
    REQUIRE(renderer.ReadMultiMeshInstance(0x77u, 1, 7, world));
    CHECK(world.m[3][1] == doctest::Approx(0.0f)); // untouched, as a static set must be
    REQUIRE(renderer.ReadMultiMeshInstance(0x77u, 0, 7, world));
    CHECK(world.m[3][1] == doctest::Approx(0.0f));
}

TEST_CASE("multimesh: a set with an uploadCount holds its whole list from the first frame, the draw prefix never re-uploads")
{
    // The vegetation contract (renderer.md): the GPU buffer holds the full chunk once; only the
    // draw count moves with distance.
    Systems s;
    if (!s.Init())
    {
        MESSAGE("no shader compiler; skipping");
        return;
    }
    shaders::ShaderSystem shaderSystem(*s.compiler, s.device);
    materials::PipelineStateCache psoCache(shaderSystem, s.device);
    materials::MaterialSystem materialSystem;
    REQUIRE(materialSystem.Initialize(s.device).IsOk());
    MeshRenderer renderer(DefaultAllocator(), s.device, shaderSystem, psoCache, materialSystem, /*framesInFlight*/ 2);
    REQUIRE(renderer.Initialize().IsOk());

    RefPtr<geometry::StaticMesh> cube = geometry::Primitives::Cube(DefaultAllocator(), 1.0f);
    RefPtr<materials::Material> material =
        materials::MaterialBuilder(u8"lit").Shader(u8"forward").Build();
    Array<Float4x4> transforms;
    for (u32 i = 0; i < 8; ++i)
    {
        transforms.PushBack(Float4x4::Translation(Float3{static_cast<f32>(i), 0.0f, 0.0f}));
    }
    auto frame = [&](u32 index, u32 drawCount)
    {
        ExtractedScene scene{DefaultAllocator()};
        AddSet(scene, 0x99u, cube.Get(), material.Get(),
               Span<const Float4x4>{transforms.Data(), drawCount}, renderer.RendererId(),
               /*version*/ 1, /*uploadCount*/ 8);
        renderer.PrepareFrame(2, index % 2);
        renderer.UploadMultiMeshes(scene);
    };
    Float4x4 world;
    frame(0, 3); // far: draws three, holds eight
    REQUIRE(renderer.ReadMultiMeshInstance(0x99u, 0, 7, world));
    CHECK(world.m[3][0] == doctest::Approx(7.0f));
    frame(1, 3);
    // The camera comes closer: the prefix grows to eight under the same version. Nothing is
    // rewritten - a change the renderer was not told about stays invisible to it.
    transforms[7].m[3][1] = 9.0f;
    frame(2, 8);
    frame(3, 8);
    REQUIRE(renderer.ReadMultiMeshInstance(0x99u, 0, 7, world));
    CHECK(world.m[3][1] == doctest::Approx(0.0f));
    REQUIRE(renderer.ReadMultiMeshInstance(0x99u, 1, 7, world));
    CHECK(world.m[3][1] == doctest::Approx(0.0f));
}

TEST_CASE("multimesh: a faded set uploads each instance's rank in its tint alpha; an unfaded set keeps its tint")
{
    // The vertex shaders dissolve an instance whose rank is above the density at its own
    // distance (instance_fade.hlsli): the rank is its position in the set's random order.
    Systems s;
    if (!s.Init())
    {
        MESSAGE("no shader compiler; skipping");
        return;
    }
    shaders::ShaderSystem shaderSystem(*s.compiler, s.device);
    materials::PipelineStateCache psoCache(shaderSystem, s.device);
    materials::MaterialSystem materialSystem;
    REQUIRE(materialSystem.Initialize(s.device).IsOk());
    MeshRenderer renderer(DefaultAllocator(), s.device, shaderSystem, psoCache, materialSystem, /*framesInFlight*/ 2);
    REQUIRE(renderer.Initialize().IsOk());
    RefPtr<geometry::StaticMesh> cube = geometry::Primitives::Cube(DefaultAllocator(), 1.0f);
    RefPtr<materials::Material> material =
        materials::MaterialBuilder(u8"lit").Shader(u8"forward").Build();
    Array<Float4x4> transforms;
    transforms.Resize(4, Float4x4::Identity());

    ExtractedScene scene{DefaultAllocator()};
    AddSet(scene, 0x51u, cube.Get(), material.Get(), Span<const Float4x4>{transforms.Data(), 4},
           renderer.RendererId());
    MultiMeshRenderData* faded = scene.Add<MultiMeshRenderData>();
    REQUIRE(faded != nullptr);
    faded->multiMesh = true;
    faded->key = 0x52u;
    faded->transforms = transforms.Data();
    faded->instanceCount = 2; // the prefix: the far half is already out
    faded->uploadCount = 4;   // the whole list carries its ranks
    faded->version = 1;
    faded->mesh = cube.Get();
    faded->material = material.Get();
    faded->rendererId = renderer.RendererId();
    faded->category = RenderCategories::Opaque;
    faded->worldRadius = 2.0f;
    faded->fadeStart = 40.0f;
    faded->fadeEnd = 80.0f;
    renderer.PrepareFrame(2, 0);
    renderer.UploadMultiMeshes(scene);

    Color tint;
    REQUIRE(renderer.ReadMultiMeshInstanceTint(0x51u, 0, 0, tint));
    CHECK(tint.a == doctest::Approx(1.0f)); // unfaded: the shared colour, alpha untouched
    for (u32 i = 0; i < 4; ++i)
    {
        REQUIRE(renderer.ReadMultiMeshInstanceTint(0x52u, 0, i, tint));
        CHECK(tint.a == doctest::Approx((static_cast<f32>(i) + 0.5f) / 4.0f)); // its rank
    }
}

