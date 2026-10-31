// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// ThumbnailStage implementation (see ThumbnailStage.cppm). The heavy render, scene and RHI
// imports live here behind Impl, as in PreviewViewportImpl.

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"

module editor.preview;

import foundation.core;
import foundation.runtime;
import foundation.runtime.client;
import foundation.graphics;
import foundation.rhi;
import foundation.image;
import foundation.scene;
import engine.scene;
import foundation.render;
import engine.render;
import foundation.resource;
import editor.core;

using namespace foundation::core;

namespace editor
{
    namespace rhi = foundation::rhi;
    namespace render = foundation::render;
    namespace image = foundation::image;
    namespace scene = foundation::scene;

    namespace
    {
        constexpr u32 kTileSize = kThumbnailServiceThumbnailSize;
        constexpr u32 kRenderSize = kTileSize * ThumbnailStage::kSupersample;

        // Exact integer box downscale (kSupersample x kSupersample average per output texel)
        // over RGBA16Float source rows. A float target holds linear values (the tonemap encodes
        // for display once, wherever the target is shown), so the average is taken in linear
        // light and the result sRGB encoded to bytes; alpha is coverage and stays linear.
        void Downscale(const u8* src, u32 srcRowBytes, image::Image& out)
        {
            constexpr u32 kFactor = ThumbnailStage::kSupersample;
            out = image::Image(kTileSize, kTileSize, image::PixelFormat::RGBA8);
            u8* dst = out.PixelDataMut().Data();
            for (u32 y = 0; y < kTileSize; ++y)
            {
                for (u32 x = 0; x < kTileSize; ++x)
                {
                    f32 sum[4] = {0, 0, 0, 0};
                    for (u32 sy = 0; sy < kFactor; ++sy)
                    {
                        const u8* row = src + static_cast<usize>(y * kFactor + sy) * srcRowBytes +
                                        static_cast<usize>(x) * kFactor * 8u;
                        for (u32 sx = 0; sx < kFactor; ++sx)
                        {
                            const u16* texel = reinterpret_cast<const u16*>(row + sx * 8u);
                            sum[0] += image::HalfToFloat(texel[0]);
                            sum[1] += image::HalfToFloat(texel[1]);
                            sum[2] += image::HalfToFloat(texel[2]);
                            sum[3] += image::HalfToFloat(texel[3]);
                        }
                    }
                    constexpr f32 kInvSamples = 1.0f / (kFactor * kFactor);
                    u8* texel = dst + (static_cast<usize>(y) * kTileSize + x) * 4u;
                    for (u32 c = 0; c < 4; ++c)
                    {
                        const f32 value = Clamp(sum[c] * kInvSamples, 0.0f, 1.0f);
                        texel[c] = static_cast<u8>((c == 3 ? value : LinearToSrgb(value)) * 255.0f +
                                                   0.5f);
                    }
                }
            }
        }
    }

    struct ThumbnailStage::Impl
    {
        enum class State : u8
        {
            Idle,          // no job; Update polls the service
            Staging,       // generator populating the scene (Pending until resources resolve)
            RenderPending, // staged and framed; the next Render declares the scene view
            CopyPending,   // view declared; the graph renders at EndRendering, after the frame
                           // encoder's commands, so the readback copy waits for the next
                           // frame's encoder to be ordered behind the render
            AwaitReadback, // copy submitted on frame `submittedIndex`; retired when the ring returns to it
        };

        runtime::IApplicationHost* host = nullptr;
        ThumbnailService* service = nullptr;
        foundation::resource::ResourceManager* resources = nullptr;
        engine::scene::SceneSubsystem* scenes = nullptr;
        engine::render::RenderSubsystem* render = nullptr;
        scene::SceneManager sceneManager{editor::EditorRootAllocator()}; // the stage's own scene group (never simulated)
        scene::Scene* scene = nullptr;

        // GPU objects (created lazily on the first Render with a live device).
        rhi::Device* device = nullptr;
        rhi::Texture* target = nullptr;
        rhi::TextureView* targetView = nullptr;
        rhi::Buffer* readback = nullptr;
        rhi::ResourceState targetState = rhi::ResourceState::Undefined;

        State state = State::Idle;
        SceneThumbnailJob job{};
        scene::Scene* jobScene = nullptr; // the shared stage, or a per-job private scene
        bool jobSceneIsPrivate = false;
        ThumbnailFraming framing;
        u32 stagingFrames = 0;
        u32 submittedIndex = 0;
        bool sawOtherIndex = false; // the ring must leave the submitted slot before the readback retires
        bool warnedDisabled = false; // the wiring-bug warning is given once (see Update)

        [[nodiscard]] bool EnsureGpuObjects()
        {
            if (target != nullptr)
            {
                return true;
            }
            if (host->Graphics() == nullptr)
            {
                return false;
            }
            device = host->Graphics()->Raw();
            if (device == nullptr)
            {
                return false;
            }
            rhi::TextureDesc desc;
            desc.width = kRenderSize;
            desc.height = kRenderSize;
            desc.format = rhi::TextureFormat::RGBA16Float;
            desc.usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::CopySrc;
            if (!device->CreateTexture(desc, target).IsOk() || target == nullptr)
            {
                target = nullptr;
                return false;
            }
            if (!device->CreateTextureView(target, rhi::TextureViewDesc{}, targetView).IsOk() ||
                targetView == nullptr)
            {
                device->DestroyTexture(target);
                target = nullptr;
                return false;
            }
            rhi::BufferDesc bufferDesc;
            bufferDesc.size = static_cast<u64>(kRenderSize) * kRenderSize * 8u;
            bufferDesc.usage = rhi::BufferUsage::CopyDst;
            bufferDesc.memory = rhi::MemoryLocation::GpuToCpu;
            if (!device->CreateBuffer(bufferDesc, readback).IsOk() || readback == nullptr)
            {
                device->DestroyTextureView(targetView);
                device->DestroyTexture(target);
                targetView = nullptr;
                target = nullptr;
                readback = nullptr;
                return false;
            }
            targetState = rhi::ResourceState::Undefined;
            return true;
        }

        // Unstages and drops the job's scene, destroying a private one. Every path that ends a job
        // goes through here, so a private scene never outlives its job.
        void ReleaseJobScene()
        {
            if (job.generator != nullptr && jobScene != nullptr)
            {
                job.generator->Unstage(*jobScene);
            }
            if (jobSceneIsPrivate && jobScene != nullptr)
            {
                sceneManager.DestroyScene(jobScene);
            }
            jobScene = nullptr;
            jobSceneIsPrivate = false;
        }

        void FailJob()
        {
            ReleaseJobScene();
            service->AcceptSceneResult(job.id, image::Image{}, false);
            job = {};
            state = State::Idle;
        }
    };

    ThumbnailStage::ThumbnailStage(runtime::IApplicationHost& host, ThumbnailService& service,
                                   foundation::resource::ResourceManager* resources)
        : m_impl(MakeUnique<Impl>(editor::EditorRootAllocator()))
    {
        m_impl->host = &host;
        m_impl->service = &service;
        m_impl->resources = resources;
        m_impl->scenes = host.Subsystems().GetSubsystem<engine::scene::SceneSubsystem>();
        m_impl->render = host.Subsystems().GetSubsystem<engine::render::RenderSubsystem>();
        if (m_impl->scenes != nullptr)
        {
            m_impl->scenes->RegisterManager(&m_impl->sceneManager);
            m_impl->scene = m_impl->sceneManager.CreateScene(u8"thumbnails.stage");
            m_impl->scene->SetSimulationEnabled(false);
        }
    }

    ThumbnailStage::~ThumbnailStage()
    {
        Shutdown();
    }

    bool ThumbnailStage::IsBusy() const
    {
        return m_impl->state != Impl::State::Idle;
    }

    void ThumbnailStage::Update()
    {
        Impl& impl = *m_impl;
        if (impl.scene == nullptr || impl.render == nullptr || impl.resources == nullptr)
        {
            // A disabled stage with queued work is a wiring bug (a null resource manager at
            // construction stalls every job silently), so say so once.
            if (!impl.warnedDisabled && impl.service->QueuedSceneJobs() > 0)
            {
                impl.warnedDisabled = true;
                LOG_WARNING(u8"Thumbnails",
                            u8"stage disabled (scene {} / renderer {} / resources {}) with {} "
                            u8"queued jobs - GPU thumbnails will not generate",
                            impl.scene != nullptr, impl.render != nullptr,
                            impl.resources != nullptr, impl.service->QueuedSceneJobs());
            }
            return;
        }

        if (impl.state == Impl::State::Idle)
        {
            SceneThumbnailJob job = impl.service->TakeSceneJob();
            if (job.id.IsNil() || job.generator == nullptr)
            {
                return;
            }
            impl.job = job;
            impl.jobSceneIsPrivate = job.generator->NeedsPrivateScene();
            if (impl.jobSceneIsPrivate)
            {
                // Inactive, so the manager never ticks it; the stage renders it and, for prewarm,
                // ticks it by hand. The registered installer assembles the app's full manager set.
                impl.jobScene = impl.sceneManager.CreateScene(u8"thumbnails.job", false);
                if (impl.jobScene == nullptr)
                {
                    impl.jobSceneIsPrivate = false;
                    impl.FailJob();
                    return;
                }
                impl.jobScene->SetSimulationEnabled(false);
            }
            else
            {
                impl.jobScene = impl.scene;
            }
            impl.stagingFrames = 0;
            impl.state = Impl::State::Staging;
        }

        if (impl.state == Impl::State::Staging)
        {
            ThumbnailFraming framing;
            const ThumbnailStageStep step =
                impl.job.generator->Stage(impl.job.id, *impl.jobScene, *impl.resources, framing);
            switch (step)
            {
            case ThumbnailStageStep::Ready:
                impl.framing = framing;
                impl.framing.radius = Max(framing.radius, 0.001f);
                impl.state = Impl::State::RenderPending;
                break;
            case ThumbnailStageStep::Failed:
                impl.FailJob();
                break;
            case ThumbnailStageStep::Pending:
                if (++impl.stagingFrames > kMaxStagingFrames)
                {
                    LOG_WARNING(u8"Thumbnails", u8"stage timed out waiting on resources for {}",
                                impl.job.id);
                    impl.FailJob();
                }
                break;
            }
        }
    }

    void ThumbnailStage::Render(foundation::graphics::FrameContext& frame)
    {
        Impl& impl = *m_impl;
        if (!frame.valid || frame.encoder == nullptr || impl.render == nullptr ||
            !impl.render->IsReady() || impl.scene == nullptr)
        {
            return;
        }

        if (impl.state == Impl::State::AwaitReadback)
        {
            // Retire when the device ring has left the submission's slot and returned to it. By
            // then the submission and its copy have completed, so the map cannot stall the frame.
            if (frame.frameIndex != impl.submittedIndex)
            {
                impl.sawOtherIndex = true;
                return;
            }
            if (!impl.sawOtherIndex)
            {
                return;
            }
            image::Image pixels;
            if (const u8* mapped = static_cast<const u8*>(impl.readback->Map()))
            {
                Downscale(mapped, kRenderSize * 8u, pixels);
                impl.readback->Unmap();
                impl.ReleaseJobScene();
                impl.service->AcceptSceneResult(impl.job.id, Move(pixels), true);
                impl.job = {};
                impl.state = Impl::State::Idle;
            }
            else
            {
                impl.FailJob();
            }
            return;
        }

        if (impl.state == Impl::State::CopyPending)
        {
            // Last frame's EndRendering executed the graph and left the target in CopySrc;
            // this frame's encoder is ordered after that submission, so the copy sees the
            // finished render.
            rhi::BufferTextureCopyRegion region;
            region.bytesPerRow = kRenderSize * 8u;
            region.rowsPerImage = kRenderSize;
            region.textureExtent = rhi::Extent3D{kRenderSize, kRenderSize, 1};
            frame.encoder->CopyTextureToBuffer(impl.target, impl.readback, region);
            impl.submittedIndex = frame.frameIndex;
            impl.sawOtherIndex = false;
            impl.state = Impl::State::AwaitReadback;
            return;
        }
        if (impl.state != Impl::State::RenderPending)
        {
            return;
        }
        if (!impl.EnsureGpuObjects())
        {
            return; // no device yet; retry next frame
        }

        // Content that is empty at t=0 (particles) asks for simulation ticks before its one
        // render. This only happens on a private scene: simulation is on for the burst and off
        // again after.
        if (impl.framing.prewarmSteps > 0 && impl.jobSceneIsPrivate)
        {
            impl.jobScene->SetSimulationEnabled(true);
            const u32 steps = Min(impl.framing.prewarmSteps, 600u);
            for (u32 i = 0; i < steps; ++i)
            {
                impl.jobScene->Update(1.0f / 60.0f);
            }
            impl.jobScene->SetSimulationEnabled(false);
        }

        // The stage never ticks a simulation otherwise, but transforms must be current for
        // extraction (generators set entity transforms during Stage).
        impl.jobScene->UpdateTransforms();

        // Ortho along (1,1,1) aimed at the framing center, sized from the staged bounds.
        const Float3 direction = Normalized(Float3{1.0f, 1.0f, 1.0f});
        const f32 distance = impl.framing.radius * 4.0f;
        const f32 halfExtent = impl.framing.radius * 1.1f;
        render::ViewCamera camera;
        const Float3 eye = impl.framing.center + direction * distance;
        camera.view = Float4x4::LookAtRH(eye, impl.framing.center, Float3{0.0f, 1.0f, 0.0f});
        camera.projection = Float4x4::OrthographicRH(halfExtent * 2.0f, halfExtent * 2.0f, 0.05f,
                                                     distance + impl.framing.radius * 4.0f);
        camera.position = eye;
        camera.farZ = distance + impl.framing.radius * 4.0f;

        render::CameraOverride cameraOverride;
        cameraOverride.camera = camera;
        cameraOverride.clearColor = Color{0.10f, 0.11f, 0.13f, 1.0f}; // linear (a render view)

        // Scene documents look like themselves: their own primary camera and clear color when
        // one exists (RenderScene extracts both when no override is passed).
        render::ViewCamera sceneCamera;
        const bool useSceneCamera =
            impl.framing.preferSceneCamera &&
            engine::render::ExtractPrimaryCamera(*impl.jobScene, sceneCamera, nullptr);

        render::TargetState targetState;
        targetState.texture = impl.target;
        targetState.currentState = impl.targetState;
        targetState.finalState = rhi::ResourceState::CopySrc;

        impl.render->RenderScene(*impl.jobScene, impl.targetView, rhi::TextureFormat::RGBA16Float,
                                 kRenderSize, kRenderSize,
                                 render::ViewportRect{0, 0, kRenderSize, kRenderSize},
                                 useSceneCamera ? nullptr : &cameraOverride, targetState);
        impl.targetState = rhi::ResourceState::CopySrc;
        impl.state = Impl::State::CopyPending;
    }

    void ThumbnailStage::Shutdown()
    {
        Impl& impl = *m_impl;
        if (impl.state != Impl::State::Idle)
        {
            impl.ReleaseJobScene();
        }
        impl.job = {};
        impl.state = Impl::State::Idle;

        if (impl.device != nullptr)
        {
            impl.device->WaitIdle(); // an in-flight readback copy may still reference these
            if (impl.readback != nullptr)
            {
                impl.device->DestroyBuffer(impl.readback);
                impl.readback = nullptr;
            }
            if (impl.targetView != nullptr)
            {
                impl.device->DestroyTextureView(impl.targetView);
                impl.targetView = nullptr;
            }
            if (impl.target != nullptr)
            {
                impl.device->DestroyTexture(impl.target);
                impl.target = nullptr;
            }
            impl.device = nullptr;
        }

        if (impl.scene != nullptr && impl.scenes != nullptr)
        {
            impl.sceneManager.Clear();
            impl.scenes->UnregisterManager(&impl.sceneManager);
            impl.scene = nullptr;
        }
    }
}
