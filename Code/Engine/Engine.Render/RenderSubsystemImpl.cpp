// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Engine::Render - the `:subsystem` partition.
///
/// RenderSubsystem: the Context-level driver that connects scenes to the (scene-agnostic)
/// renderer. It owns the GPU systems - the DXC compiler, ShaderSystem, PipelineStateCache,
/// the MeshRenderer + RendererRegistry, and the per-frame RenderFrame driver - and, as an
/// As an ISceneObserver it reacts to scene teardown; assembly via the scene composition.
///
/// It implements ISceneRenderer (Begin/RenderScene×N/End): the app's render callback brackets
/// the frame with BeginRendering/EndRendering and calls RenderScene per active scene. Each
/// RenderScene extracts the scene into an ExtractedScene snapshot and collects a RenderView;
/// EndRendering composes all views. (The built-in forward shader binds no material set on its
/// own; material binding is handled separately.)

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"
#include "Profiler/Profiler.h"

module engine.render;
import engine.domain;
import foundation.geometry.resource;
import foundation.model.resource;
import foundation.materials.resource;
import foundation.texture.resource;
import foundation.image.resource;
import foundation.shaders.resource;

import foundation.core;
import foundation.rhi;
import foundation.profiler;
import foundation.runtime;         // Subsystem, Context
import foundation.scene; // Scene
import engine.scene; // SceneSubsystem (to register as scene-aware)
import foundation.shaders.system;  // ShaderSystem (borrowed from the host)
import foundation.materials;       // MaterialSystem
import foundation.materials.pipelinecache;   // PipelineStateCache
import foundation.render;          // MeshRenderer, RendererRegistry, RenderFrame, ExtractedScene
import :components;
import :extract;
import :scene_renderer;

using namespace foundation::core;
using namespace foundation::render;
namespace materials = foundation::materials;
namespace rhi = foundation::rhi;

namespace engine::render
{
    // Foundation alias (sibling engine::scene would otherwise shadow foundation::scene).
    namespace scene = foundation::scene;

    u16 RenderSubsystem::RegisterRenderer(Renderer& renderer)
    {
        m_registry.Register(&renderer);
        return renderer.RendererId();
    }

    void RenderSubsystem::RegisterProvider(scene::Scene& scene, IRenderDataProvider& provider)
    {
        m_providers.PushBack(SceneProvider{&scene, &provider});
    }

    void RenderSubsystem::Update(f32 deltaTime)
    {
        if (m_frame.Get() != nullptr)
        {
            m_frame->SetDeltaSeconds(deltaTime);
        }
    }

    i32 RenderSubsystem::UpdateOrder() const noexcept
    {
        return 1000;
    } // late (renders, doesn't tick)

    void AddRenderSceneManagers(scene::Scene& scene)
    {
        scene.AddSystem<MeshComponentManager>();
        scene.AddSystem<InstancedMeshComponentManager>();
        scene.AddSystem<SpriteComponentManager>();
        scene.AddSystem<DecalComponentManager>();
        scene.AddSystem<CameraComponentManager>();
        scene.AddSystem<LightComponentManager>();
        scene.AddSystem<ReflectionProbeComponentManager>();
        scene.AddSystem<EnvironmentSystem>();
        scene.AddSystem<PostProcessSystem>();
    }

    void RenderSubsystem::OnDestroying(scene::Scene& scene)
    {
        usize w = 0;
        for (usize r = 0; r < m_providers.Size(); ++r)
        {
            if (m_providers[r].scene != &scene)
            {
                m_providers[w++] = m_providers[r];
            }
        }
        m_providers.Resize(w);
        // Evict the scene's debug-draw list: the map is keyed on the raw Scene*, and script can
        // mint entries freely (DebugDraw.of(scene)), so without this every level reload leaks one
        // entry - and a recycled Scene* address would silently adopt the dead scene's list.
        m_debugScenes.Remove(&scene);
    }

    void RenderSubsystem::SetSkyEquirect(u32 w, u32 h, Span<const f32> rgba)
    {
        if (m_iblSystem.Get() != nullptr)
        {
            m_iblSystem->SetEquirect(w, h, rgba);
        }
    }

    void RenderSubsystem::SetSkyCubemap(u32 faceSize, Span<const u8> sixFaces)
    {
        if (m_iblSystem.Get() != nullptr)
        {
            m_iblSystem->SetCubemap(faceSize, sixFaces);
        }
    }

    void RenderSubsystem::SetExposure(f32 exposure) noexcept
    {
        m_exposure = exposure;
        m_globalPostActive = true;
    }

    void RenderSubsystem::SetBloomEnabled(bool on) noexcept
    {
        m_bloomEnabled = on;
        m_globalPostActive = true;
    }

    void RenderSubsystem::SetBloomIntensity(f32 v) noexcept
    {
        m_bloomIntensity = v;
        m_globalPostActive = true;
    }

    void RenderSubsystem::SetBloomThreshold(f32 v) noexcept
    {
        m_bloomThreshold = v;
        m_globalPostActive = true;
    }

    void RenderSubsystem::SetAoMode(AoMode m) noexcept
    {
        m_aoMode = m;
        m_globalPostActive = true;
    }

    void RenderSubsystem::SetAoStrength(f32 v) noexcept
    {
        m_aoStrength = v;
        m_globalPostActive = true;
    }

    void RenderSubsystem::SetAoRadius(f32 v) noexcept
    {
        m_aoRadius = v;
        m_globalPostActive = true;
    }

    void RenderSubsystem::SetAoIntensity(f32 v) noexcept
    {
        m_aoIntensity = v;
        m_globalPostActive = true;
    }

    void RenderSubsystem::SetAoDebug(i32 mode) noexcept
    {
        m_aoDebug = mode;
    } // 0=off, 1=AO, 2/3/4=N.xyz, 5=viewZ, 6=depth

    void RenderSubsystem::SetSsrEnabled(bool on) noexcept
    {
        m_ssrEnabled = on;
        m_globalPostActive = true;
    }
    void RenderSubsystem::SetMsaaSamples(u32 count) noexcept
    {
        m_globalMsaaSamples = (count < 1) ? 1u : count; // clamped to the device ceiling per view
        m_globalPostActive = true;
    }

    bool RenderSubsystem::SupportsMsaaSamples(u32 count) const noexcept
    {
        if (count <= 1)
        {
            return true; // 1x (off) is always available
        }
        // MSAA needs the resolve pass; without it (or a device) only 1x is usable. Otherwise defer to
        // the device's exact-count support AND the queried ceiling.
        if (m_msaaResolvePass.Get() == nullptr || m_device == nullptr)
        {
            return false;
        }
        return count <= m_maxMsaaSamples && m_device->SupportsSampleCount(count);
    }

    void RenderSubsystem::ViewCullStats(u32& culled, u32& total) const noexcept
    {
        if (m_frame.Get() != nullptr)
        {
            m_frame->CullStats(culled, total);
        }
        else
        {
            culled = 0;
            total = 0;
        }
    }

    void RenderSubsystem::SetFxaaEnabled(bool on) noexcept
    {
        m_fxaaEnabled = on;
        m_globalPostActive = true;
    }

    void RenderSubsystem::SetFxaaSubpixel(f32 v) noexcept
    {
        m_fxaaSubpixel = v;
        m_globalPostActive = true;
    }

    debug::DebugDraw& RenderSubsystem::DebugScene(scene::Scene& s)
    {
        if (debug::DebugDraw* p = m_debugScenes.Find(&s))
        {
            return *p;
        }
        return m_debugScenes.InsertOrAssign(&s, debug::DebugDraw{});
    }

    debug::DebugDraw& RenderSubsystem::DebugView(const void* viewportKey)
    {
        if (debug::DebugDraw* p = m_debugViews.Find(viewportKey))
        {
            return *p;
        }
        return m_debugViews.InsertOrAssign(viewportKey, debug::DebugDraw{});
    }

    void RenderSubsystem::SetTaaEnabled(bool on) noexcept
    {
        m_taaEnabled = on;
        m_globalPostActive = true;
    }

    void RenderSubsystem::SetTaaBlend(f32 v) noexcept
    {
        m_taaBlend = v;
        m_globalPostActive = true;
    }

    void RenderSubsystem::SetTaaGamma(f32 v) noexcept
    {
        m_taaGamma = v;
        m_globalPostActive = true;
    }

    void RenderSubsystem::BuildGpuProfileReport(String& out)
    {
        if (m_frame.Get() == nullptr || m_device == nullptr)
        {
            return;
        }
        m_device->WaitIdle();
        m_frame->ReadGpuProfile(out);
    }

    void RenderSubsystem::BeginRendering(rhi::CommandEncoder& encoder, u32 frameIndex)
    {
        PROFILE_SCOPE("Render.Begin");
        if (m_frame.Get() == nullptr)
        {
            return;
        }
        // Dev hot reload (throttled inside the provider). On a reload, idle the GPU so
        // passes can destroy + rebuild their version-stamped pipelines immediately (a
        // dev-only hiccup; the material path still goes through the PSO retire ring).
        if (m_shaders != nullptr && m_shaders->PumpReloads() > 0)
        {
            m_device->WaitIdle();
        }
        m_sceneCount = 0;
        m_snapshotOwners.Resize(m_scenes.Size()); // per-frame scene tags for snapshot sharing
        m_targetScenes.Clear();
        ++m_frameNumber;
        // Provision per-worker extraction arenas for this frame (one per job-system slot, or a
        // single slot when the job system is absent - serial fallback).
        const u32 slotCount = HasGlobalJobSystem() ? GlobalJobs().SlotCount() : 1u;
        m_renderCtx.BeginFrame(slotCount);
        m_retireQueue.Tick(); // free retired GPU resources that have aged past all in-flight frames
        m_frame->SetExposure(m_exposure);
        m_frame->SetTime(m_timeSeconds); // the WIND sway clock (prev = last frame's)
        m_frame->SetBloom(m_bloomEnabled ? m_bloomIntensity : 0.0f, m_bloomThreshold, m_bloomKnee);
        m_frame->SetTaa(m_taaEnabled, m_taaBlend, m_taaGamma, m_taaMotionScale);
        // DEBUG harness: ENV_AO_DEBUG forces the AO debug channel to screen (0=off, 1=AO,
        // 2/3/4=N.xyz, 5=viewZ, 6=depth) so SSAO reconstruction can be compared across backends.
        {
            static bool s_aoDbgRead = false;
            static i32 s_aoDbg = 0;
            if (!s_aoDbgRead)
            {
                s_aoDbgRead = true;
                if (auto v = GetEnvironmentVariable(u8"ENV_AO_DEBUG");
                    v.HasValue() && !v.Value().IsEmpty())
                {
                    const char8_t c = v.Value()[0];
                    if (c >= u8'0' && c <= u8'9')
                    {
                        s_aoDbg = static_cast<i32>(c - u8'0');
                    }
                }
            }
            if (s_aoDbg != 0)
            {
                m_aoDebug = s_aoDbg;
            }
        }
        m_frame->SetAo(m_aoMode, m_aoStrength, m_aoRadius, m_aoIntensity, m_aoDebug);
        m_frame->SetFxaa(m_fxaaEnabled, m_fxaaSubpixel);
        m_frame->SetInstanceSharing(m_instanceSharing);
        m_frame->SetViewCulling(m_viewCulling);
        m_frame->SetDebug(m_debugPass.Get(), &m_debugGlobal, &m_debugScreen);
        m_frame->SetSceneOverlays(&m_sceneOverlays.Items());
        m_frame->SetDecal(m_decalPass.Get());
        m_frame->SetSsr(m_ssrPass.Get());
        m_frame->SetSsgi(m_ssgiPass.Get());
        m_frame->SetSsrParams(m_ssrEnabled, m_ssrParams);
        m_frame->SetMsaaResolve(m_msaaResolvePass.Get());
        m_frame->SetPick(&EnsurePickSystem());
        m_frame->SetProbes(m_probeSystem.Get());
        if (m_probeSystem.Get() != nullptr)
        {
            m_probeSystem->BeginFrame();
        } // records re-accumulate per scene
        m_frame->Begin(encoder, frameIndex);
    }

    void RenderSubsystem::RenderScene(scene::Scene& scene, rhi::TextureView* target,
                                      rhi::TextureFormat targetFormat, u32 width, u32 height,
                                      ViewportRect viewport, const CameraOverride* cameraOverride,
                                      const TargetState& targetState,
                                      const ViewPostOverride* postOverride, const void* viewportKey,
                                      const ViewDebugView* debugView, const SceneSize& sceneSize)
    {
        if (m_frame.Get() == nullptr || target == nullptr)
        {
            return;
        }

        // ONE extraction per scene per frame: a scene rendered through several views
        // (split-screen) shares one snapshot - so the per-scene shadow/probe/IBL work
        // grouped on the snapshot downstream runs once, not per view. (Scenes must not
        // mutate between RenderScene calls of one frame - the Begin/End bracket contract.)
        ExtractedScene* snapshot = nullptr;
        for (usize i = 0; i < m_sceneCount; ++i)
        {
            if (m_snapshotOwners[i] == &scene)
            {
                snapshot = m_scenes[i].Get();
                break;
            }
        }
        const bool firstSight = (snapshot == nullptr);

        // The view's camera, resolved BEFORE extraction: the first view's position rides on the
        // snapshot for producers that thin by distance (vegetation's fade prefix).
        ViewCamera camera;
        CameraOverride fallback;                 // the default backdrop (no primary camera)
        Color clearColor = fallback.clearColor; // linear: cameras' authored colours are decoded
        if (cameraOverride != nullptr)
        {
            camera = cameraOverride->camera;
            clearColor = cameraOverride->clearColor;
        }
        else
        {
            // The clear comes from the camera; the projection takes the shape of what the scene
            // draws at: its own size, else the viewport, else the whole target.
            f32 aspectWidth = static_cast<f32>(width);
            f32 aspectHeight = static_cast<f32>(height);
            if (sceneSize.IsSet())
            {
                aspectWidth = static_cast<f32>(sceneSize.width);
                aspectHeight = static_cast<f32>(sceneSize.height);
            }
            else if (viewport.width > 0 && viewport.height > 0)
            {
                aspectWidth = static_cast<f32>(viewport.width);
                aspectHeight = static_cast<f32>(viewport.height);
            }
            (void)ExtractPrimaryCamera(scene, camera, &clearColor,
                                       aspectHeight > 0.0f ? aspectWidth / aspectHeight : 0.0f);
        }

        if (firstSight)
        {
            snapshot = AcquireScene();
            if (m_snapshotOwners.Size() < m_sceneCount)
            {
                m_snapshotOwners.Resize(m_sceneCount);
            }
            m_snapshotOwners[m_sceneCount - 1] = &scene;
        }
        if (firstSight)
        {
            PROFILE_SCOPE("Render.Extract");
            ExtractSceneInto(scene, *snapshot,
                             m_renderCtx); // parallel when the job system is up (resets snapshot)
            snapshot->SetViewOrigin(camera.position); // after the reset, before the providers
            snapshot->SetSceneSerial(scene.Serial()); // a new scene restarts the view's history
            ExtractInstancedMeshesInto(
                scene, *snapshot); // instanced sets (MultiMesh): one item each, O(1)/frame
            if (m_spriteRenderer.Get() != nullptr)
            { // billboards (into the same snapshot, after meshes)
                ExtractSpritesInto(scene, *snapshot, m_spriteRenderer->RendererId());
            }
            ExtractDecalsInto(scene,
                              *snapshot); // screen-space decals (DecalPass, not the Renderer path)
            ExtractLightsInto(scene, *snapshot); // lights are shading inputs, not draws
            ExtractReflectionProbesInto(scene,
                                        *snapshot); // reflection probes (capture/prefilter inputs)
            ExtractEnvironmentInto(scene, *snapshot); // per-scene ambient
            // Downstream systems (particles, world-UI, ...) registered for THIS scene contribute into
            // the same snapshot - render stays ignorant of their types (scene-free IRenderDataProvider).
            for (const SceneProvider& sp : m_providers)
            {
                if (sp.scene == &scene && sp.provider != nullptr)
                {
                    sp.provider->ExtractRenderData(*snapshot);
                }
            }
            if (m_probeSystem.Get() != nullptr)
            { // map probes to persistent array slots (capture in P1b)
                m_probeSystem->Assign(snapshot, snapshot->ReflectionProbes());
            }
        }
        // After extraction, so the snapshot's view origin is this view's camera, not a target's.
        RenderTargetCameras(scene);

        ViewSettings settings;
        settings.clear = rhi::ClearColor{clearColor.r, clearColor.g, clearColor.b, clearColor.a};
        settings.viewportX = viewport.x;
        settings.viewportY = viewport.y;
        settings.viewportWidth = viewport.width;
        settings.viewportHeight = viewport.height;
        settings.scene = sceneSize;
        settings.targetTexture = targetState.texture;
        settings.targetCurrentState = targetState.currentState;
        settings.targetFinalState = targetState.finalState;

        // Resolve this view's post-processing (exposure/bloom/AO). The programmatic global override
        // (samples' debug panels) wins once touched; otherwise the scene's authored PostProcessSettings
        // drive - exposure authored in EV/stops is resolved to the tonemap's linear multiplier here.
        // (AA/SSR remain frame-global.)
        if (m_globalPostActive)
        {
            settings.post.exposure = m_exposure;
            settings.post.agxTonemap = true; // the legacy global API has no operator toggle
            settings.post.bloomEnabled = m_bloomEnabled;
            settings.post.bloomThreshold = m_bloomThreshold;
            settings.post.bloomKnee = m_bloomKnee;
            settings.post.bloomIntensity = m_bloomIntensity;
            settings.post.aoMode = static_cast<u32>(m_aoMode);
            settings.post.aoStrength = m_aoStrength;
            settings.post.aoRadius = m_aoRadius;
            settings.post.aoIntensity = m_aoIntensity;
            settings.post.taaEnabled = m_taaEnabled;
            settings.post.taaBlend = m_taaBlend;
            settings.post.taaGamma = m_taaGamma;
            settings.post.fxaaEnabled = m_fxaaEnabled;
            settings.post.fxaaSubpixel = m_fxaaSubpixel;
            settings.post.ssrEnabled = m_ssrEnabled;
            settings.post.ssrIntensity = m_ssrParams.intensity;
            settings.post.msaaSamples = static_cast<u8>(m_globalMsaaSamples);
        }
        else if (const PostProcessSystem* pp = scene.GetSystem<PostProcessSystem>())
        {
            settings.post = ResolveScenePost(pp->Effective()); // the scene's, or its profile's
        }
        // Editor viewport "show flags": ephemeral per-view overrides that strip effects for editing
        // clarity, layered ON TOP of the resolved post - never written back to the scene.
        if (postOverride != nullptr)
        {
            ApplyViewPostOverride(settings.post, *postOverride);
            settings.frustumCull = !postOverride->disableCulling;
        }
        // Editor debug view: pass-through selection (validated per view at declare time -
        // an unknown resource name simply shows the final image).
        if (debugView != nullptr)
        {
            settings.debug = *debugView;
        }
        if (projection::IsOrthographic(camera.projection))
        {
            LimitPostForOrthographic(settings.post);
        }
        settings.viewportKey = viewportKey; // pick requests bind to it
        settings.sceneOverlays = !m_renderingTargets;
        // Finalize per-view motion-vector need AFTER any override: TAA OR an SSR temporal pass.
        // SSR's `temporal` stays frame-global, so the OR lands here, not in ResolveScenePost.
        settings.post.needsMotion =
            settings.post.taaEnabled || (settings.post.ssrEnabled && m_ssrParams.temporal) ||
            settings.post.ssgiEnabled; // SSGI's temporal resolve reprojects by velocity
        // Scene-pass MSAA: snap the AUTHORED intent to what the device supports.
        // The valid set is NOT [1 .. ceiling]: WebGPU supports only {1, 4}, never 2. So clamp to the
        // ceiling, then snap DOWN to the nearest device-supported count (2x on WebGPU degrades to 1x;
        // 4x stays 4x). An unsupported count reaching texture/pipeline creation aborts the device, so
        // this snap - not just a ceiling clamp - is what keeps a 2x request from crashing on web.
        {
            u32 s = settings.post.msaaSamples;
            if (s < 1)
            {
                s = 1;
            }
            if (s > m_maxMsaaSamples)
            {
                s = m_maxMsaaSamples;
            }
            while (s > 1 && !m_device->SupportsSampleCount(s))
            {
                s >>= 1;
            }
            settings.post.msaaSamples = static_cast<u8>(s);
        }
        {
            PROFILE_SCOPE("Render.AddView"); // binds the view + builds/sorts its draw list
            // EVERY view of a scene draws that scene's per-scene list (physics/nav debug - what
            // PIE shows). A KEYED view additionally draws its OWN DebugView list (editor
            // grid/selection gizmos), which never appears in another view of the same scene -
            // the #118 camera-preview contract. The pre-fix either/or here made keyed views
            // (the edit viewport) silently drop ALL scene-level debug draw. Either pointer may
            // be null when nothing was drawn this frame - the debug pass null-checks.
            // A target camera's view (a minimap, a monitor) is the game's picture, not a debug
            // view: the scene's debug lines stay out of it.
            const void* sceneDebug =
                m_renderingTargets ? nullptr : static_cast<const void*>(m_debugScenes.Find(&scene));
            const void* viewDebug = (viewportKey != nullptr)
                                        ? static_cast<const void*>(m_debugViews.Find(viewportKey))
                                        : nullptr;
            m_frame->AddView(*snapshot, camera, settings, target, targetFormat, width, height,
                             sceneDebug,
                             /*sceneKey*/ &scene, viewDebug);
        }
    }

    void RenderSubsystem::RenderTargetCameras(scene::Scene& scene)
    {
        if (m_renderingTargets)
        {
            return; // a target's own view: its scene is already being handled
        }
        for (scene::Scene* done : m_targetScenes)
        {
            if (done == &scene)
            {
                return;
            }
        }
        m_targetScenes.PushBack(&scene);
        CollectTargetCameras(scene, m_frameNumber, m_targetViews);
        if (m_targetViews.IsEmpty())
        {
            return;
        }
        m_renderingTargets = true; // the views below render no targets of their own
        for (const TargetCameraView& view : m_targetViews)
        {
            // The factory leaves a render texture shader-readable, and so does every render into
            // it, so each render starts and ends there.
            TargetState state;
            state.texture = view.target->GpuTexture();
            state.currentState = rhi::ResourceState::ShaderRead;
            state.finalState = rhi::ResourceState::ShaderRead;
            // No TAA, bloom, AO or SSR: a small second view stays cheap and out of the per-view
            // history (exposure and the tonemap stay, so it still displays).
            ViewPostOverride post;
            post.disablePost = true;
            RenderScene(scene, view.target->View(), view.target->Format(), view.target->Width(),
                        view.target->Height(), {}, &view.camera, state, &post);
        }
        m_renderingTargets = false;
    }

    void RenderSubsystem::GetDebugResources(Array<DebugResourceInfo>& out)
    {
        out.Clear();
        for (const DebugResourceInfo& row : m_debugResourceSnapshot)
        {
            out.PushBack(row);
        }
    }

    void RenderSubsystem::EndRendering()
    {
        PROFILE_SCOPE("Render.Compose");
        if (m_frame.Get() != nullptr)
        {
            m_frame->End();
            // Snapshot the frame's graph-texture inventory while the graph still holds it
            // (Begin resets the graph) - the editor's debug-view picker reads this copy.
            m_frame->CollectDebugResources(m_debugResourceSnapshot);
        }
        // Immediate-mode: clear all debug lists AFTER rendering, so next frame's draws start empty
        // (the app accumulates during its update, before the next BeginRendering).
        m_debugGlobal.Clear();
        m_debugScreen.Clear();
        for (auto& kv : m_debugScenes)
        {
            kv.value.Clear();
        }
        for (auto& kv : m_debugViews)
        {
            kv.value.Clear();
        }
    }

    void RenderSubsystem::UnregisterOverlay(IScreenOverlay* overlay)
    {
        m_screenOverlays.Remove(overlay);
    }

    void RenderSubsystem::RenderOverlays(rhi::CommandEncoder& encoder, rhi::TextureView* target,
                                         rhi::TextureFormat targetFormat, u32 width, u32 height,
                                         u32 frameIndex)
    {
        if (target == nullptr || width == 0 || height == 0 || m_screenOverlays.IsEmpty())
        {
            return;
        }
        // Stencil attachment for overlay UI (stencil-then-cover fills). Cached at the
        // target size; a resize retires the old texture (in-flight frames may still
        // reference it) and recreates. Cleared every frame, so Undefined -> write.
        if (!m_overlayDsProbed)
        {
            m_overlayDsFormat = PickStencilFormat(*m_device);
            m_overlayDsProbed = true;
        }
        if (m_overlayDsFormat != rhi::TextureFormat::Undefined &&
            (m_overlayDsTexture == nullptr || m_overlayDsWidth != width ||
             m_overlayDsHeight != height))
        {
            m_retireQueue.Retire(m_overlayDsView);
            m_retireQueue.Retire(m_overlayDsTexture);
            m_overlayDsView = nullptr;
            m_overlayDsTexture = nullptr;
            rhi::TextureDesc dsDesc{};
            dsDesc.dimension = rhi::TextureDimension::Texture2D;
            dsDesc.format = m_overlayDsFormat;
            dsDesc.width = width;
            dsDesc.height = height;
            dsDesc.depth = 1;
            dsDesc.usage = rhi::TextureUsage::DepthStencil;
            dsDesc.label = u8"screen.overlay.ds";
            if (m_device->CreateTexture(dsDesc, m_overlayDsTexture).IsOk() &&
                m_overlayDsTexture != nullptr)
            {
                if (!m_device->CreateTextureView(m_overlayDsTexture, rhi::TextureViewDesc{},
                                                 m_overlayDsView)
                         .IsOk())
                {
                    m_retireQueue.Retire(m_overlayDsTexture);
                    m_overlayDsTexture = nullptr;
                    m_overlayDsView = nullptr;
                }
            }
            m_overlayDsWidth = width;
            m_overlayDsHeight = height;
        }
        rhi::RenderPassDesc pass;
        rhi::ColorAttachment color;
        color.view = target;
        color.loadOp = rhi::LoadOp::Load;
        color.storeOp = rhi::StoreOp::Store;
        pass.colorAttachments.Add(color);
        const bool haveDs = m_overlayDsView != nullptr;
        if (haveDs)
        {
            // The backend does not auto-transition pass attachments; fully cleared, so
            // previous contents are discardable and Undefined is the correct source.
            encoder.TransitionTexture(m_overlayDsTexture, rhi::ResourceState::Undefined,
                                      rhi::ResourceState::DepthStencilWrite);
            rhi::DepthStencilAttachment ds{};
            ds.view = m_overlayDsView;
            ds.depthLoadOp = rhi::LoadOp::Clear;
            ds.depthStoreOp = rhi::StoreOp::DontCare;
            ds.stencilLoadOp = rhi::LoadOp::Clear; // stencil-then-cover expects 0
            ds.stencilStoreOp = rhi::StoreOp::DontCare;
            ds.stencilClearValue = 0;
            pass.depthStencilAttachment = ds;
        }
        if (rhi::RenderPassEncoder* rp = encoder.BeginRenderPass(pass))
        {
            ScreenOverlayView view;
            view.width = width;
            view.height = height;
            view.targetFormat = targetFormat;
            view.depthStencilFormat = haveDs ? m_overlayDsFormat : rhi::TextureFormat::Undefined;
            view.frameIndex = frameIndex;
            for (IScreenOverlay* overlay : m_screenOverlays.Items())
            {
                overlay->Render(*rp, view);
            }
            rp->End();
        }
    }

    void RenderSubsystem::OnInit()
    {
        RegisterRenderComponentReflection(); // tooling: reflected components (idempotent)

        // The ShaderSystemHost encapsulates the pack-vs-dev decision over the data mount (cooked
        // Shaders/shaders.dpak => no compiler; otherwise DXC + a file provider over Shaders/ with
        // hot reload). The same host every consumer (VG/UI, ImGui) uses. Inert if neither is present.
        if (!m_shaderHost.Initialize(*m_device, *m_dataFileSystem))
        {
            return; // neither a compiler nor a pack - renderer stays inert
        }
        m_shaders = m_shaderHost.System();
        const rhi::ShaderFormat shaderFmt = m_device->PreferredShaderFormat();
        const char* const shaderFmtName = shaderFmt == rhi::ShaderFormat::WGSL   ? "WGSL"
                                          : shaderFmt == rhi::ShaderFormat::DXIL  ? "DXIL"
                                                                                  : "SPIR-V";
        if (m_shaderHost.UsingPack())
        {
            rhi::LogInfof("RenderSubsystem: using cooked shader pack (%u variants, %s) - no runtime "
                          "compiler",
                          static_cast<unsigned>(m_shaderHost.PackVariantCount()), shaderFmtName);
        }
        else
        {
            rhi::LogInfof("RenderSubsystem: using runtime shader compiler (%s)", shaderFmtName);
            if (shaderFmt == rhi::ShaderFormat::WGSL)
            {
                // The runtime compiler (DXC) cannot emit WGSL - that is a cook-time path (naga).
                // Every shader lookup will miss and the scene renders BLACK. The usual cause is
                // ENV_WEBGPU_WGSL=1 without OPTION_USE_SHADER_PACK=1 (or no cooked
                // Shaders/shaders.dpak in the data root).
                rhi::LogErrorf("RenderSubsystem: the device wants WGSL but there is no cooked "
                               "shader pack - the runtime compiler cannot produce WGSL, so "
                               "NOTHING will render. Cook a WGSL Shaders/shaders.dpak into the "
                               "data root and set OPTION_USE_SHADER_PACK=1.");
            }
        }

        m_psoCache =
            MakeUnique<materials::PipelineStateCache>(m_allocator, *m_shaders, *m_device);
        m_materialSystem = MakeUnique<materials::MaterialSystem>(m_allocator);
        if (!m_materialSystem->Initialize(*m_device).IsOk())
        {
            m_materialSystem.Reset();
            return;
        }

        m_meshRenderer = MakeUnique<MeshRenderer>(m_allocator, *m_device, *m_shaders,
                                                  *m_psoCache, *m_materialSystem, m_framesInFlight);
        if (!m_meshRenderer->Initialize().IsOk())
        {
            m_meshRenderer.Reset();
            return;
        }
        m_registry.Register(
            m_meshRenderer.Get()); // FIRST -> renderer id 0 (the RenderData default)
        // Frames-in-flight retire queue: every grow-path replacement retires through it
        // instead of a mid-frame WaitIdle (which on web pumps the event loop, expires the
        // canvas texture, and drops the frame's submit - the dropped-submit class).
        m_retireQueue.Initialize(m_device, static_cast<i32>(m_framesInFlight));
        m_meshRenderer->SetRetireQueue(&m_retireQueue);

        // Sprites: registered after the mesh renderer (id 1); shares the blended forward pass.
        m_spriteRenderer =
            MakeUnique<SpriteRenderer>(m_allocator, *m_device, *m_shaders, m_framesInFlight);
        if (m_spriteRenderer->Initialize().IsOk())
        {
            m_registry.Register(m_spriteRenderer.Get());
            m_spriteRenderer->SetRetireQueue(&m_retireQueue);
        }
        else
        {
            m_spriteRenderer.Reset();
        }

        // Debug toggles: flip to false to isolate a subsystem (e.g. bisecting a rendering bug). When
        // off, the renderer falls back gracefully - clustering off => the shader's all-lights path;
        // shadows off => unshadowed. Kept as compile-time flags (zero cost when on).
        constexpr bool kEnableClusters = true;
        constexpr bool kEnableShadows = true;

        // Clustered light culling: a build compute pass per view (declared into the frame graph by
        // RenderFrame). Optional - if it fails to init, the renderer runs without clustering.
        if (kEnableClusters)
        {
            m_clusterSystem = MakeUnique<ClusterSystem>(m_allocator, *m_device, *m_shaders,
                                                        m_framesInFlight);
            if (!m_clusterSystem->Initialize().IsOk())
            {
                m_clusterSystem.Reset();
            }
            if (m_clusterSystem)
            {
                m_clusterSystem->SetRetireQueue(&m_retireQueue);
            }
        }

        // HDR resolve: forward renders linear HDR, this pass tonemaps to the LDR target. Optional -
        // if it fails to init, the renderer falls back to writing the LDR target directly.
        m_tonemapPass =
            MakeUnique<TonemapPass>(m_allocator, *m_device, *m_shaders, m_framesInFlight);
        if (!m_tonemapPass->Initialize().IsOk())
        {
            m_tonemapPass.Reset();
        }

        // Directional shadow map. Optional - if it fails to init, the scene renders unshadowed.
        if (kEnableShadows)
        {
            m_shadowSystem =
                MakeUnique<ShadowSystem>(m_allocator, *m_device, m_framesInFlight);
            if (!m_shadowSystem->Initialize().IsOk())
            {
                m_shadowSystem.Reset();
            }
            if (m_shadowSystem)
            {
                m_shadowSystem->SetRetireQueue(&m_retireQueue);
            }
        }

        // Image-based lighting. Optional - if it fails to init, the scene uses flat ambient.
        m_iblSystem = MakeUnique<IBLSystem>(m_allocator, *m_device, *m_shaders);
        if (!m_iblSystem->Initialize().IsOk())
        {
            m_iblSystem.Reset();
        }

        m_probeSystem =
            MakeUnique<ReflectionProbeSystem>(m_allocator, *m_device, *m_shaders);
        if (!m_probeSystem->Initialize().IsOk())
        {
            m_probeSystem.Reset();
        }

        // Visible sky (background) from the IBL environment. Optional.
        m_skyPass =
            MakeUnique<SkyPass>(m_allocator, *m_device, *m_shaders, m_framesInFlight);
        if (!m_skyPass->Initialize().IsOk())
        {
            m_skyPass.Reset();
        }

        // HDR bloom (composited at tonemap). Optional.
        m_bloomPass = MakeUnique<BloomPass>(m_allocator, *m_device, *m_shaders);
        if (!m_bloomPass->Initialize().IsOk())
        {
            m_bloomPass.Reset();
        }

        // Temporal AA resolve (per-view history). Optional.
        m_taaPass = MakeUnique<TaaPass>(m_allocator, *m_device, *m_shaders);
        if (!m_taaPass->Initialize().IsOk())
        {
            m_taaPass.Reset();
        }

        // Ambient occlusion (GTAO/SSAO from the G-buffer). Optional.
        m_aoPass = MakeUnique<AoPass>(m_allocator, *m_device, *m_shaders);
        if (!m_aoPass->Initialize().IsOk())
        {
            m_aoPass.Reset();
        }

        // Screen-space reflections (reflect the lit HDR before AO/TAA). Optional.
        m_ssrPass = MakeUnique<SsrPass>(m_allocator, *m_device, *m_shaders);
        if (!m_ssrPass->Initialize().IsOk())
        {
            m_ssrPass.Reset();
        }

        // Screen-space GI (tier 1, the diffuse SSR twin). Optional.
        m_ssgiPass = MakeUnique<SsgiPass>(m_allocator, *m_device, *m_shaders);
        if (!m_ssgiPass->Initialize().IsOk())
        {
            m_ssgiPass.Reset();
        }

        // Scene-pass MSAA first-sample resolve. Optional - null leaves MSAA unavailable
        // (views clamp to 1x), so the engine still renders without it.
        m_msaaResolvePass = MakeUnique<MsaaResolvePass>(m_allocator, *m_device, *m_shaders);
        if (!m_msaaResolvePass->Initialize().IsOk())
        {
            m_msaaResolvePass.Reset();
        }
        // Scene-pass MSAA device ceiling: the max sample count the device supports
        // for color+depth, capped at 4 by the query. Views clamp their authored intent to this. If the
        // resolve pass failed to init, MSAA is unavailable regardless, so force 1x.
        m_maxMsaaSamples =
            (m_msaaResolvePass.Get() != nullptr) ? m_device->MaxColorDepthSampleCount() : 1u;
        LOG_INFO(u8"Render", u8"scene-pass MSAA: resolve-pass={} device-ceiling={}x",
                 (m_msaaResolvePass.Get() != nullptr) ? u8"ok" : u8"FAILED", m_maxMsaaSamples);

        // FXAA (TAA-off fallback AA). Optional.
        m_fxaaPass =
            MakeUnique<FxaaPass>(m_allocator, *m_device, *m_shaders, m_framesInFlight);
        if (!m_fxaaPass->Initialize().IsOk())
        {
            m_fxaaPass.Reset();
        }

        // Editor debug-view blit (visualize any graph texture in a viewport). Optional.
        m_debugBlitPass =
            MakeUnique<DebugBlitPass>(m_allocator, *m_device, *m_shaders, m_framesInFlight);
        if (!m_debugBlitPass->Initialize().IsOk())
        {
            m_debugBlitPass.Reset();
        }

        // Screen-space decals (project onto depth, blend into HDR before AO/TAA). Optional.
        m_decalPass =
            MakeUnique<DecalPass>(m_allocator, *m_device, *m_shaders, m_framesInFlight);
        if (!m_decalPass->Initialize().IsOk())
        {
            m_decalPass.Reset();
        }
        if (m_decalPass)
        {
            m_decalPass->SetRetireQueue(&m_retireQueue);
        }

        // Debug draw (per-view gizmos + screen text). Optional.
        m_debugPass =
            MakeUnique<DebugDrawPass>(m_allocator, *m_device, *m_shaders, m_framesInFlight);
        if (!m_debugPass->Initialize().IsOk())
        {
            m_debugPass.Reset();
        }

        m_exposurePass =
            MakeUnique<ExposurePass>(m_allocator, *m_device, *m_shaders, m_framesInFlight);
        if (!m_exposurePass->Initialize().IsOk())
        {
            m_exposurePass.Reset(); // auto-exposure silently unavailable; fixed EV still works
        }

        m_frame = MakeUnique<RenderFrame>(
            m_allocator, m_allocator, *m_device, m_registry, m_framesInFlight,
            m_clusterSystem.Get(),
            m_tonemapPass.Get(), m_shadowSystem.Get(), m_iblSystem.Get(), m_skyPass.Get(),
            m_bloomPass.Get(), m_taaPass.Get(), m_aoPass.Get(), m_fxaaPass.Get(),
            m_exposurePass.Get(), m_debugBlitPass.Get());
        m_frame->EnableGpuProfiling(); // per-pass GPU timestamps (cheap; read on the P-key dump)
    }

    void RenderSubsystem::OnReady()
    {
        // Register as a scene observer (reactive provider cleanup).
        if (foundation::runtime::Context* ctx = GetContext())
        {
            if (auto* scenes = ctx->GetSubsystem<engine::scene::SceneSubsystem>())
            {
                scenes->RegisterObserver(this, scene::SceneLifecycleStage::Destroying);
            }
        }
    }

    void RenderSubsystem::OnShutdown()
    {
        if (foundation::runtime::Context* ctx = GetContext())
        {
            if (auto* scenes = ctx->GetSubsystem<engine::scene::SceneSubsystem>())
            {
                scenes->UnregisterObserver(this);
            }
        }
        m_device->WaitIdle();     // GPU must finish before we free its buffers/PSOs/descriptors
        m_retireQueue.Flush();    // pending retired resources (GPU idle - free now)
        if (m_overlayDsView != nullptr)
        {
            m_device->DestroyTextureView(m_overlayDsView);
            m_overlayDsView = nullptr;
        }
        if (m_overlayDsTexture != nullptr)
        {
            m_device->DestroyTexture(m_overlayDsTexture);
            m_overlayDsTexture = nullptr;
        }
        m_frame.Reset();          // releases the forward pass's per-frame GPU resources
        m_pickSystem.Reset();     // pick readback buffers (device idle above)
        m_clusterSystem.Reset();  // before the ShaderSystem it borrows
        m_tonemapPass.Reset();    // before the ShaderSystem it borrows
        m_exposurePass.Reset();   // before the ShaderSystem it borrows (auto-exposure, with tonemap)
        m_shadowSystem.Reset();   // shadow depth textures
        m_skyPass.Reset();        // before the ShaderSystem it borrows
        m_bloomPass.Reset();      // before the ShaderSystem it borrows
        m_taaPass.Reset();        // before the ShaderSystem it borrows
        m_aoPass.Reset();         // before the ShaderSystem it borrows
        m_ssrPass.Reset();        // before the ShaderSystem it borrows
        m_ssgiPass.Reset();       // before the ShaderSystem it borrows (with SSR)
        m_fxaaPass.Reset();       // before the ShaderSystem it borrows
        m_decalPass.Reset();      // before the ShaderSystem it borrows
        m_debugPass.Reset();      // before the ShaderSystem it borrows
        m_debugBlitPass.Reset();  // before the ShaderSystem it borrows (with the debug pass)
        m_probeSystem.Reset();    // probe textures/buffers (before the ShaderSystem it borrows)
        m_iblSystem.Reset();      // IBL textures/buffers (before the ShaderSystem it borrows)
        m_spriteRenderer.Reset(); // before the ShaderSystem it borrows
        m_meshRenderer.Reset(); // before the systems it borrows (releases material instances first)
        m_materialSystem.Reset();
        m_psoCache.Reset();
        m_shaders = nullptr;      // borrowed from the host; the host owns/destroys the ShaderSystem
        m_shaderHost.Shutdown();  // after every pass that borrowed *m_shaders
    }

    PickSystem& RenderSubsystem::EnsurePickSystem()
    {
        if (m_pickSystem.Get() == nullptr)
        {
            m_pickSystem = MakeUnique<PickSystem>(m_allocator, m_allocator, *m_device, m_framesInFlight);
            m_pickSystem->SetRetireQueue(&m_retireQueue);
        }
        return *m_pickSystem;
    }

    PickRequestId RenderSubsystem::RequestPick(const void* viewportKey, i32 x, i32 y, u32 width,
                                               u32 height)
    {
        if (viewportKey == nullptr)
        {
            return kInvalidPickRequest; // nothing could ever answer it
        }
        return EnsurePickSystem().Request(viewportKey, PickRect{x, y, width, height});
    }

    bool RenderSubsystem::TryTakePickResult(PickRequestId id, PickResult& out)
    {
        return m_pickSystem.Get() != nullptr && m_pickSystem->TryTakeResult(id, out);
    }

    bool RenderSubsystem::IsPickPending(PickRequestId id) const noexcept
    {
        return m_pickSystem.Get() != nullptr && m_pickSystem->IsPending(id);
    }

    void RenderSubsystem::CancelPicks(const void* viewportKey)
    {
        if (m_pickSystem.Get() != nullptr)
        {
            m_pickSystem->Cancel(viewportKey);
        }
    }

    ExtractedScene* RenderSubsystem::AcquireScene()
    {
        if (m_sceneCount == m_scenes.Size())
        {
            m_scenes.PushBack(MakeUnique<ExtractedScene>(m_allocator, m_allocator));
        }
        ExtractedScene* s = m_scenes[m_sceneCount++].Get();
        s->Reset();
        return s;
    }
}

namespace engine::render
{
    const engine::DomainModule& RenderDomain() noexcept
    {
        static const foundation::resource::ResourceModule* const kResources[] = {
            &foundation::geometry::kGeometryResourceModule,
            &foundation::model::kModelResourceModule,
            &foundation::materials::kMaterialsResourceModule,
            &foundation::texture::kTextureResourceModule,
            &foundation::image::kImageResourceModule,
            &foundation::shaders::kShadersResourceModule,
            &kRenderProfileResourceModule};
        static const engine::DomainModule kModule{
            .id = u8"render",
            .installScene = &AddRenderSceneManagers,
            .registerReflection = &RegisterRenderComponentReflection,
            .registerScriptFacade = &RegisterRenderScriptFacade,
            .resources = foundation::core::Span<const foundation::resource::ResourceModule* const>{
                kResources, sizeof(kResources) / sizeof(kResources[0])}};
        return kModule;
    }
}
