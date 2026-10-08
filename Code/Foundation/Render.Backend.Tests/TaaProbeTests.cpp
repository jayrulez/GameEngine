// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// TAA under a moving camera, proven at the pixel on real Vulkan and WebGPU devices through the full
// RenderFrame chain. Bright bars on a dark wall; the camera slides sideways exactly one pixel's
// width a frame, so each frame is the last one shifted a pixel. With exact motion vectors the
// resolved image moves with the scene and stays as steady as under a still camera; with wrong
// ones the history lands in the wrong place, the clip throws it away, and the jittered frame shows
// through (PaperKid's chase camera: steady titles, jittery levels). And with a second view drawn
// before it on alternate frames (PaperKid's minimap, a render texture every other frame), the main
// view keeps its own history by its key, not by its place in the frame's list. And with the camera
// still and the bars one skinned mesh slid a pixel a frame by its bone, its motion vectors come from
// last frame's pose, so it resolves as steadily, drawn alone (the single path) or batched.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.rhi;
import foundation.rhi.vulkan;
import foundation.rhi.webgpu;
import foundation.rhi.testsupport;
import foundation.geometry;
import foundation.materials;
import foundation.materials.pipelinecache;
import foundation.shaders;
import foundation.shaders.system;
import foundation.render;
import foundation.rendergraph;

using namespace foundation::core;
using namespace foundation::render;
namespace rhi = foundation::rhi;
namespace testsupport = foundation::rhi::testsupport;
namespace geometry = foundation::geometry;
namespace materials = foundation::materials;
namespace shaders = foundation::shaders;

namespace
{
    constexpr u32 kSize = 128;
    constexpr f32 kWallDistance = 5.0f;
    constexpr f32 kFov = 1.0472f; // 60 degrees
    constexpr u32 kFrames = 32;   // TAA settles over the first ones
    constexpr u32 kKept = 6;      // the last frames, read back and compared

    // The world width one pixel covers on the wall: one frame's camera step.
    f32 PixelStep() { return 2.0f * kWallDistance * Tan(kFov * 0.5f) / static_cast<f32>(kSize); }

    struct Item
    {
        RefPtr<geometry::StaticMesh> mesh;
        RefPtr<materials::Material> material;
        Float4x4 world = Float4x4::Identity();
        Float3 center = Float3{0, 0, 0};
        f32 radius = 1.0f;
    };

    Item Slab(Float3 size, Float3 at, RefPtr<materials::Material> material)
    {
        Item item;
        item.mesh = geometry::Primitives::Cube(DefaultAllocator(), 1.0f);
        item.material = Move(material);
        item.world = Float4x4::Scale(size) * Float4x4::Translation(at);
        item.center = at;
        item.radius = Length(size);
        return item;
    }

    // A second view drawn on alternate frames BEFORE the main one, into a target of its own
    // (TAA off, as a render texture's camera); `keyed` gives both views a history key, as the render
    // subsystem does, else they are known by their place in the list.
    struct SideView
    {
        bool on = false;
        bool keyed = false;
    };

    // The bars as ONE skinned mesh, every vertex on bone 0: moving the bone moves them all.
    RefPtr<geometry::SkinnedMesh> SkinnedBars()
    {
        RefPtr<geometry::SkinnedMesh> bars = MakeRef<geometry::SkinnedMesh>(DefaultAllocator());
        RefPtr<geometry::StaticMesh> cube = geometry::Primitives::Cube(DefaultAllocator(), 1.0f);
        const u32 barIndices = cube->indices.Count();
        bars->indices.Resize(41u * barIndices);
        geometry::VertexSkinning one{};
        one.weights = Float4{1, 0, 0, 0};
        for (i32 b = -20; b <= 20; ++b)
        {
            const u32 base = static_cast<u32>(bars->vertices.Size());
            const Float3 at{0.3f * static_cast<f32>(b), 0.0f, -kWallDistance + 0.01f};
            for (geometry::StaticMeshVertex v : cube->vertices)
            {
                v.position = Float3{v.position.x * 0.1f, v.position.y * 20.0f, v.position.z * 0.02f} + at;
                bars->vertices.PushBack(v);
                bars->skinning.PushBack(one);
            }
            for (u32 k = 0; k < barIndices; ++k)
            {
                bars->indices.Add(base + cube->indices.Get(k));
            }
        }
        bars->bounds = AABB::FromCenterExtents(Float3{0, 0, -kWallDistance}, Float3{7.0f, 10.0f, 0.1f});
        bars->subMeshes.PushBack(geometry::SubMesh{0, static_cast<i32>(bars->indices.Count()), 0, geometry::PrimitiveType::Triangles});
        return bars;
    }

    // Render kFrames with the camera stepping `step` along +X a frame (TAA on or off) and read back
    // the last kKept. `skinnedStep`: the camera stays and the bars, one skinned mesh, slide that far
    // along -X a frame by their bone.
    Array<testsupport::CapturedImage> RenderSlide(rhi::Device& device, f32 step, bool taaOn, SideView side = {},
                                                  f32 skinnedStep = 0.0f, bool twin = false)
    {
        Array<testsupport::CapturedImage> kept;
        shaders::ShaderSystemHost host{DefaultAllocator()};
        if (!host.Initialize(device, testsupport::DataFileSystem()))
        {
            return kept;
        }
        {
            shaders::ShaderSystem& shaderSystem = *host.System();
            materials::PipelineStateCache psoCache(shaderSystem, device);
            materials::MaterialSystem materialSystem;
            REQUIRE(materialSystem.Initialize(device).IsOk());
            MeshRenderer meshRenderer(DefaultAllocator(), device, shaderSystem, psoCache, materialSystem, 2);
            REQUIRE(meshRenderer.Initialize().IsOk());
            RendererRegistry registry;
            registry.Register(&meshRenderer);
            TonemapPass tonemap(device, shaderSystem, 2);
            REQUIRE(tonemap.Initialize().IsOk());
            TaaPass taa(device, shaderSystem);
            REQUIRE(taa.Initialize().IsOk());

            RenderFrame frame(DefaultAllocator(), device, registry, 2, /*clusters*/ nullptr, &tonemap,
                              /*shadows*/ nullptr, /*ibl*/ nullptr, /*sky*/ nullptr, /*bloom*/ nullptr,
                              taaOn ? &taa : nullptr, /*ao*/ nullptr, /*fxaa*/ nullptr);

            // A dark wall and bright vertical bars just in front of it, 0.3 m apart, 0.1 m wide.
            Array<Item> items;
            RefPtr<materials::Material> dark = materials::CreatePBR(u8"taa.wall", Float4{0.05f, 0.05f, 0.05f, 1.0f}, 0.0f, 0.9f);
            RefPtr<materials::Material> bright = materials::CreatePBR(u8"taa.bar", Float4{0.9f, 0.9f, 0.9f, 1.0f}, 0.0f, 0.9f);
            items.PushBack(Slab(Float3{40.0f, 20.0f, 0.1f}, Float3{0.0f, 0.0f, -kWallDistance - 0.05f}, dark));
            for (i32 b = -20; b <= 20 && skinnedStep == 0.0f; ++b)
            {
                items.PushBack(Slab(Float3{0.1f, 20.0f, 0.02f}, Float3{0.3f * static_cast<f32>(b), 0.0f, -kWallDistance + 0.01f},
                                    bright));
            }
            ExtractedScene scene{DefaultAllocator()};
            scene.SetAmbient(Float3{1.0f, 1.0f, 1.0f});
            // Each draw is its own entity: the renderer keeps each one's previous world matrix (motion
            // vectors) by entity id, so draws sharing an id would take each other's.
            u64 nextEntity = 1;
            for (const Item& item : items)
            {
                MeshRenderData* data = scene.Add<MeshRenderData>();
                data->entityId = nextEntity++;
                data->world = item.world;
                data->worldCenter = item.center;
                data->worldRadius = item.radius;
                data->mesh = item.mesh.Get();
                data->material = item.material.Get();
                data->category = RenderCategories::Opaque;
            }
            RefPtr<geometry::SkinnedMesh> bars;
            Array<Float4x4> palette, previous; // this frame's pose and last frame's (motion vectors)
            palette.Resize(1, Float4x4::Identity());
            previous.Resize(1, Float4x4::Identity());
            if (skinnedStep != 0.0f)
            {
                bars = SkinnedBars();
                MeshRenderData* data = scene.Add<MeshRenderData>();
                data->entityId = nextEntity++;
                data->world = Float4x4::Identity();
                data->worldCenter = Float3{0.0f, 0.0f, -kWallDistance};
                data->worldRadius = 20.0f;
                data->mesh = bars.Get();
                data->material = bright.Get();
                data->category = RenderCategories::Opaque;
                // As extraction keys it: the draw list groups by this, so the twin below sorts beside
                // it and the two batch (by depth alone, the wall lands between them).
                data->sortBatchKey = BatchKey(bars.Get(), bright.Get());
                data->boneMatrices = palette.Data(); // the same palettes every frame, their poses moving
                data->prevBoneMatrices = previous.Data();
                data->boneCount = 1;
                if (twin) // a second copy hidden behind the wall: the two batch, so the instanced path
                {
                    MeshRenderData* copy = scene.Add<MeshRenderData>();
                    *copy = *data;
                    copy->entityId = nextEntity++;
                    copy->world = Float4x4::Translation(Float3{0.0f, 0.0f, -6.0f});
                    copy->worldCenter = Float3{0.0f, 0.0f, -6.0f - kWallDistance};
                }
            }

            rhi::TextureDesc td{};
            td.format = rhi::TextureFormat::RGBA8Unorm;
            td.width = kSize;
            td.height = kSize;
            td.usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::CopySrc;
            td.label = u8"taa.probe.target";
            rhi::Texture* target = nullptr;
            REQUIRE(device.CreateTexture(td, target).IsOk());
            rhi::TextureViewDesc vd{};
            vd.format = rhi::TextureFormat::RGBA8Unorm;
            rhi::TextureView* targetView = nullptr;
            REQUIRE(device.CreateTextureView(target, vd, targetView).IsOk());
            td.label = u8"taa.probe.side";
            rhi::Texture* sideTarget = nullptr;
            REQUIRE(device.CreateTexture(td, sideTarget).IsOk());
            rhi::TextureView* sideView = nullptr;
            REQUIRE(device.CreateTextureView(sideTarget, vd, sideView).IsOk());

            rhi::CommandPool* pool = nullptr;
            REQUIRE(device.CreateCommandPool(rhi::QueueType::Graphics, pool).IsOk());
            rhi::Fence* fence = nullptr;
            REQUIRE(device.CreateFence(0, fence).IsOk());
            rhi::Queue* queue = device.GetQueue(rhi::QueueType::Graphics);
            REQUIRE(queue != nullptr);

            ViewSettings settings;
            settings.clear = rhi::ClearColor::Black();
            settings.targetTexture = target;
            settings.targetFinalState = rhi::ResourceState::CopySrc;
            settings.post.bloomEnabled = false;
            settings.post.taaEnabled = taaOn;
            settings.post.needsMotion = taaOn;
            settings.historyKey = side.keyed ? 1u : 0u;
            ViewSettings sideSettings;
            sideSettings.clear = rhi::ClearColor::Black();
            sideSettings.targetTexture = sideTarget;
            sideSettings.targetFinalState = rhi::ResourceState::CopySrc;
            sideSettings.post.bloomEnabled = false;
            sideSettings.post.taaEnabled = false;
            sideSettings.historyKey = side.keyed ? 2u : 0u;
            bool sideDrawn = false;
            ViewCamera sideCamera; // looking down at the wall from above: nothing like the main view
            sideCamera.position = Float3{0.0f, 20.0f, -kWallDistance};
            sideCamera.view = Float4x4::LookAtRH(sideCamera.position, Float3{0.0f, 0.0f, -kWallDistance}, Float3{0, 0, -1});
            sideCamera.projection = Float4x4::PerspectiveFovRH(kFov, 1.0f, 0.1f, 100.0f);

            for (u32 i = 0; i < kFrames; ++i)
            {
                previous[0] = palette[0];
                palette[0] = Float4x4::Translation(Float3{-skinnedStep * static_cast<f32>(i), 0.0f, 0.0f});
                const Float3 eye{step * static_cast<f32>(i), 0.0f, 0.0f};
                ViewCamera camera;
                camera.position = eye;
                camera.view = Float4x4::LookAtRH(eye, eye + Float3{0, 0, -1}, Float3{0, 1, 0});
                camera.projection = Float4x4::PerspectiveFovRH(kFov, 1.0f, 0.1f, 100.0f);

                rhi::CommandEncoder* encoder = nullptr;
                REQUIRE(pool->CreateEncoder(encoder).IsOk());
                settings.targetCurrentState = (i == 0) ? rhi::ResourceState::Undefined : rhi::ResourceState::CopySrc;
                frame.SetDeltaSeconds(1.0f / 60.0f);
                frame.Begin(*encoder, i % 2);
                if (side.on && i % 2 == 0)
                {
                    sideSettings.targetCurrentState = sideDrawn ? rhi::ResourceState::CopySrc : rhi::ResourceState::Undefined;
                    sideDrawn = true;
                    frame.AddView(scene, sideCamera, sideSettings, sideView, rhi::TextureFormat::RGBA8Unorm, kSize, kSize);
                }
                frame.AddView(scene, camera, settings, targetView, rhi::TextureFormat::RGBA8Unorm, kSize, kSize);
                frame.End();
                rhi::CommandBuffer* commandBuffer = encoder->Finish();
                REQUIRE(commandBuffer != nullptr);
                rhi::CommandBuffer* commandBuffers[] = {commandBuffer};
                queue->Submit(Span<rhi::CommandBuffer* const>(commandBuffers, 1), fence, i + 1);
                REQUIRE(fence->Wait(i + 1, ~0ull));
                pool->DestroyEncoder(encoder);
                if (i + kKept >= kFrames)
                {
                    kept.PushBack(testsupport::Readback(device, target, kSize, kSize));
                }
            }

            device.WaitIdle();
            device.DestroyFence(fence);
            device.DestroyCommandPool(pool);
            device.DestroyTextureView(sideView);
            device.DestroyTexture(sideTarget);
            device.DestroyTextureView(targetView);
            device.DestroyTexture(target);
        }
        host.Shutdown();
        return kept;
    }

    // How far the resolved image strays from moving with the scene: the mean difference between
    // each frame and the one before shifted `shift` pixels left (the scene's motion), over the
    // interior columns.
    f32 MeanStray(const Array<testsupport::CapturedImage>& frames, u32 shift)
    {
        f64 sum = 0.0;
        u32 n = 0;
        for (usize f = 1; f < frames.Size(); ++f)
        {
            for (u32 y = 8; y + 8 < kSize; ++y)
            {
                for (u32 x = 8; x + 8 < kSize; ++x)
                {
                    const u8* now = frames[f].At(x, y);
                    const u8* before = frames[f - 1].At(x + shift, y);
                    for (u32 c = 0; c < 3; ++c)
                    {
                        sum += (now[c] > before[c]) ? (now[c] - before[c]) : (before[c] - now[c]);
                    }
                    ++n;
                }
            }
        }
        return n > 0 ? static_cast<f32>(sum / n) : 0.0f;
    }

    void ProbeTaaSlide(rhi::Device& device, const char* backend)
    {
        CAPTURE(backend);
        const Array<testsupport::CapturedImage> stillTaa = RenderSlide(device, 0.0f, true);
        const Array<testsupport::CapturedImage> movingTaa = RenderSlide(device, PixelStep(), true);
        const Array<testsupport::CapturedImage> movingRaw = RenderSlide(device, PixelStep(), false);
        REQUIRE(stillTaa.Size() == kKept);
        REQUIRE(movingTaa.Size() == kKept);
        REQUIRE(movingRaw.Size() == kKept);
        const f32 still = MeanStray(stillTaa, 0);
        const f32 moving = MeanStray(movingTaa, 1);
        const f32 raw = MeanStray(movingRaw, 1);
        MESSAGE(doctest::String(backend) << ": frame-to-frame stray, TAA still " << still << ", TAA moving " << moving
                                         << ", no TAA moving " << raw);
        // Under a camera moving one pixel a frame, the resolved image moves with the scene nearly as
        // steadily as under a still camera (resampling the history at sub-pixel positions costs a
        // little). Dropping history by motion measured in UV made it five times worse (7.7 to 1.5):
        // the jitter showed through.
        CHECK(moving < still * 2.5f);
    }

    void ProbeTaaSkinned(rhi::Device& device, const char* backend)
    {
        CAPTURE(backend);
        const f32 still = MeanStray(RenderSlide(device, 0.0f, true, {}, 1e-6f), 0);
        const f32 alone = MeanStray(RenderSlide(device, 0.0f, true, {}, PixelStep()), 1);
        const f32 batched = MeanStray(RenderSlide(device, 0.0f, true, {}, PixelStep(), /*twin*/ true), 1);
        MESSAGE(doctest::String(backend) << ": skinned bars' stray, still " << still << ", sliding a pixel a frame alone "
                                         << alone << ", batched with a twin " << batched);
        // As steady as the camera slide by either path: the motion vectors carry last frame's pose.
        // With this frame's pose for last frame's (the single path's old bone base) the bars' motion
        // read as none and the history smeared (105 against 2.9).
        CHECK(alone < still * 2.5f);
        CHECK(batched < still * 2.5f);
    }

    void ProbeTaaSideView(rhi::Device& device, const char* backend)
    {
        CAPTURE(backend);
        const f32 alone = MeanStray(RenderSlide(device, 0.0f, true), 0);
        const f32 byOrder = MeanStray(RenderSlide(device, 0.0f, true, SideView{true, false}), 0);
        const f32 byKey = MeanStray(RenderSlide(device, 0.0f, true, SideView{true, true}), 0);
        MESSAGE(doctest::String(backend) << ": still TAA stray, main view alone " << alone
                                         << ", with an alternate-frame view before it (by order) " << byOrder
                                         << ", (by key) " << byKey);
        // Keyed, the main view keeps its history whatever is drawn before it: as steady as alone.
        CHECK(byKey < alone + 0.5f);
        // By order it alternates between two places, reading the other view's camera and history.
        CHECK(byOrder > alone * 3.0f);
    }

    void ForEachBackend(void (*probe)(rhi::Device&, const char*))
    {
        rhi::Backend* vulkan = nullptr;
        (void)rhi::vk::CreateBackend(rhi::vk::VkBackendDesc{}, vulkan);
        rhi::Backend* webgpu = nullptr;
        (void)rhi::webgpu::CreateBackend(rhi::webgpu::WebGpuBackendDesc{}, webgpu, DefaultAllocator());
        bool any = false;
        if (rhi::Device* device = testsupport::MakeTestDevice(vulkan))
        {
            probe(*device, "vulkan");
            device->Destroy();
            any = true;
        }
        else
        {
            MESSAGE("Vulkan unavailable - skipped");
        }
        if (rhi::Device* device = testsupport::MakeTestDevice(webgpu))
        {
            probe(*device, "webgpu");
            device->Destroy();
            any = true;
        }
        else
        {
            MESSAGE("WebGPU unavailable - skipped");
        }
        if (!any)
        {
            MESSAGE("no GPU backend available - TAA probe skipped entirely");
        }
        if (vulkan != nullptr)
        {
            vulkan->Destroy();
        }
        if (webgpu != nullptr)
        {
            webgpu->Destroy();
        }
    }
}

TEST_CASE("taa: under a camera moving a pixel a frame, the resolved image moves with the scene")
{
    ForEachBackend(&ProbeTaaSlide);
}

TEST_CASE("taa: a view drawn before the main one on alternate frames leaves the main view's history alone")
{
    ForEachBackend(&ProbeTaaSideView);
}

TEST_CASE("taa: a lone skinned mesh slid by its bone resolves as steadily as a camera slide")
{
    ForEachBackend(ProbeTaaSkinned);
}
