// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Tools.ModelViewer - `modelviewer.thumbnails`: the thumbnails in the browse grid. Models are
// queued and drawn one at a time. Each is opened (cooked, or bound from the cache), placed in its
// rest pose in the renderer's private scene under the viewer's lighting, framed from the front and
// a little above, rendered into its own target, and read back to a PNG in the thumbnail cache
// (modelviewer::ThumbnailPathFor), which the grid shows. A model is not drawn again while its file
// is unchanged. The renderer reads model files on its own worker (FileLoads), so a folder of large
// models never stalls the window. A new queue drops any read under way, and nothing is written for
// it.
//
// The render is declared in one frame and runs at its end (after the frame encoder's own
// commands), so the copy back is recorded on the frame after, and the PNG written once the GPU has
// run it.

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"

export module modelviewer.thumbnails;

import foundation.core;
import foundation.rhi;
import foundation.rhi.capture;
import foundation.image;
import foundation.runtime;
import foundation.runtime.client;
import foundation.scene;
import foundation.render;
import foundation.geometry;
import engine.scene;
import engine.render;
import engine.model;
import editor.camera;
import modelviewer;

using namespace foundation::core;

export namespace modelviewer
{
    namespace rhi = foundation::rhi;
    namespace scene = foundation::scene;
    namespace render = foundation::render;

    class ThumbnailRenderer
    {
    public:
        /// `size` is a thumbnail's side in pixels (96, 128 or 192 in the settings).
        ThumbnailRenderer(IAllocator& allocator, foundation::runtime::IApplicationHost& host, StringView cacheRoot, StringView thumbRoot, u32 size = 128)
            : m_allocator(&allocator), m_sceneManager(allocator), m_queue(allocator), m_cacheRoot(cacheRoot), m_thumbRoot(thumbRoot), m_size(size),
              m_loads(allocator)
        {
            m_scenes = host.Subsystems().GetSubsystem<engine::scene::SceneSubsystem>();
            m_render = host.Subsystems().GetSubsystem<engine::render::RenderSubsystem>();
            m_device = host.Graphics() != nullptr ? host.Graphics()->Raw() : nullptr;
            (void)CreateDirectories(m_thumbRoot.AsView());
        }
        ~ThumbnailRenderer() { Shutdown(); }

        /// Called with each model's path and its thumbnail file once the file exists (drawn now or
        /// earlier), or with an empty file when the model did not open.
        void SetOnDone(Function<void(StringView model, StringView png)> done) { m_onDone = foundation::core::Move(done); }

        /// Replaces the queue with `models`, drawn in order.
        void SetQueue(Span<const String> models)
        {
            if (m_state == State::Reading)
            {
                // The model being read is no longer wanted: drop it and write nothing for it.
                m_loads.Cancel();
                DropModel();
                m_state = State::Idle;
            }
            m_queue.Clear();
            for (const String& model : models)
                m_queue.PushBack(model);
            m_next = 0;
        }
        /// Whether models open with loose textures (OpenModel::SetLooseTextures), matching the viewer's tabs.
        void SetLooseTextures(bool on) noexcept { m_looseTextures = on; }

        /// Draws `model` next, ahead of the queue (for an agent asking for its thumbnail).
        void Prepend(StringView model)
        {
            for (usize i = m_next; i < m_queue.Size(); ++i)
                if (m_queue[i].AsView() == model)
                {
                    m_queue.RemoveAt(i);
                    break;
                }
            m_queue.Insert(m_next, String(model));
        }

        [[nodiscard]] usize Remaining() const noexcept { return m_queue.Size() - m_next + (m_state != State::Idle ? 1u : 0u); }
        /// The queue's length and how many of it are done, for the progress line.
        [[nodiscard]] usize Queued() const noexcept { return m_queue.Size(); }
        [[nodiscard]] usize Done() const noexcept { return Remaining() > m_queue.Size() ? 0 : m_queue.Size() - Remaining(); } // a model finishing from a replaced queue does not count

        /// Called each frame before rendering: hands over drawn files and starts the next model.
        void Update()
        {
            if (m_state == State::Copied && m_capture.Recorded() && m_device != nullptr)
            {
                m_device->WaitIdle();
                foundation::image::Image written;
                const Status saved = m_capture.Complete(*m_device, *m_allocator, written);
                Finish(saved.IsOk() ? m_png.AsView() : StringView{});
            }
            if (m_state == State::Reading)
            {
                FileRead read;
                while (m_state == State::Reading && m_loads.Take(read))
                    if (read.path == m_model)
                    {
                        if (read.outcome == OpenModel::Outcome::Opened && m_open->Finish(read.model, read.stats) == OpenModel::Outcome::Opened)
                            Stage();
                        else
                        {
                            LOG_WARNING(u8"ModelViewer", u8"no thumbnail for {}: it did not open", m_model.AsView());
                            Finish({});
                        }
                    }
            }
            if (m_state == State::Staged && ++m_settleFrames >= 2)
                m_state = State::RenderPending; // the pose is applied and the transforms composed
            while (m_state == State::Idle && m_next < m_queue.Size())
                Start(m_queue[m_next++].AsView());
        }

        /// Called during the frame's rendering: declares the staged model's view, or records the
        /// copy-back of the view declared last frame.
        void Render(rhi::CommandEncoder& encoder)
        {
            if (m_state == State::CopyPending && m_target != nullptr)
            {
                m_capture.Request(m_png.AsView());
                if (m_capture.Record(*m_device, encoder, m_target, rhi::TextureFormat::RGBA16Float, m_size, m_size, rhi::ResourceState::CopySrc))
                    m_state = State::Copied;
                else
                    Finish({});
                return;
            }
            if (m_state != State::RenderPending || m_render == nullptr || !m_render->IsReady() || !EnsureTarget())
                return;
            // Look from a front quarter angle (so a long model shows its length) and a little above,
            // as close as the bounding sphere still fits the square.
            editor::EditorCamera camera;
            camera.yaw = -0.8f;
            camera.pitch = -0.3f;
            camera.position = m_center - camera.Forward() * (m_radius / Sin(camera.fovY * 0.5f) * 1.02f);
            camera.nearZ = Max(m_radius * 0.05f, 0.0001f);
            camera.farZ = Max(m_radius * 100.0f, 50.0f);
            render::ViewCamera view;
            view.view = Float4x4::LookAtRH(camera.position, camera.position + camera.Forward(), camera.Up());
            view.projection = camera.Projection(1.0f);
            view.position = camera.position;
            view.farZ = camera.farZ;
            render::CameraOverride cameraOverride;
            cameraOverride.camera = view;
            cameraOverride.clearColor = Color{0.10f, 0.11f, 0.12f, 1.0f};
            render::TargetState target;
            target.texture = m_target;
            target.currentState = m_targetState;
            target.finalState = rhi::ResourceState::CopySrc; // only copied out, never sampled
            m_render->RenderScene(*m_scene, m_targetView, rhi::TextureFormat::RGBA16Float, m_size, m_size,
                                  render::ViewportRect{0, 0, m_size, m_size}, &cameraOverride, target);
            m_targetState = rhi::ResourceState::CopySrc;
            m_state = State::CopyPending;
        }

        void Shutdown()
        {
            DropModel();
            if (m_device != nullptr)
            {
                m_capture.Release(*m_device);
                if (m_targetView != nullptr)
                    m_device->DestroyTextureView(m_targetView);
                if (m_target != nullptr)
                    m_device->DestroyTexture(m_target);
            }
            m_targetView = nullptr;
            m_target = nullptr;
            if (m_scene != nullptr)
                m_sceneManager.DestroyScene(m_scene);
            m_scene = nullptr;
            if (m_scenes != nullptr)
                m_scenes->UnregisterManager(&m_sceneManager);
            m_scenes = nullptr;
        }

    private:
        enum class State : u8
        {
            Idle,
            Reading,       // the file is being read on the worker
            Staged,        // placed in the scene; the pose and transforms settle over a frame or two
            RenderPending, // the next Render declares its view
            CopyPending,   // declared; the next Render records the copy-back
            Copied,        // the copy recorded; the next Update writes the PNG
        };

        void Start(StringView model)
        {
            m_model = String(model);
            m_png = ThumbnailPathFor(m_thumbRoot.AsView(), model);
            if (m_png.IsEmpty())
            {
                Finish({});
                return;
            }
            if (FileExists(m_png.AsView()))
            {
                Finish(m_png.AsView()); // drawn before and the file is unchanged
                return;
            }
            if (!EnsureScene())
            {
                Finish({});
                return;
            }
            m_open = MakeUnique<OpenModel>(*m_allocator, *m_allocator);
            m_open->SetLooseTextures(m_looseTextures);
            OpenModel::Outcome outcome = OpenModel::Outcome::NotFound;
            const OpenModel::Begun begun = m_open->Begin(model, m_cacheRoot.AsView(), m_device, outcome);
            if (begun == OpenModel::Begun::NeedsFile)
            {
                m_loads.Request(model, m_looseTextures);
                m_state = State::Reading;
                return;
            }
            if (begun == OpenModel::Begun::Failed)
            {
                LOG_WARNING(u8"ModelViewer", u8"no thumbnail for {}: it did not open", model);
                Finish({});
                return;
            }
            Stage();
        }

        // Places the opened model in the scene in its rest pose, to settle and then be drawn.
        void Stage()
        {
            engine::model::InstantiateOptions options;
            options.animate = false; // the rest pose
            m_instance = engine::model::InstantiateModel(*m_scene, *m_open->Model(), u8"thumbnail", options);
            m_scene->UpdateTransforms();
            Measure();
            m_settleFrames = 0;
            m_state = State::Staged;
        }

        void Finish(StringView png)
        {
            DropModel();
            m_state = State::Idle;
            if (m_onDone)
                m_onDone(m_model.AsView(), png);
        }

        void DropModel()
        {
            if (m_scene != nullptr && m_scene->IsValid(m_instance.root))
                m_scene->DestroyEntity(m_instance.root);
            m_instance = {};
            m_open.Reset();
        }

        void Measure()
        {
            m_center = Float3{};
            m_radius = 1.0f;
            auto* meshes = m_scene->GetSystem<engine::render::MeshComponentManager>();
            if (meshes == nullptr)
                return;
            AABB box = AABB::Empty();
            meshes->ForEach([&](engine::render::MeshComponent& component, scene::EntityHandle entity)
                            {
                                const foundation::geometry::StaticMesh* mesh = component.mesh.Get();
                                if (mesh != nullptr && mesh->bounds.IsValid())
                                    box = Merge(box, TransformAABB(mesh->bounds, m_scene->ComposeWorldMatrix(entity)));
                            });
            if (!box.IsValid())
                return;
            m_center = (box.min + box.max) * 0.5f;
            m_radius = Max(Length(box.max - box.min) * 0.5f, 0.01f);
        }

        // Creates the renderer's own scene, lit like the viewer's view with the procedural sky and a sun.
        bool EnsureScene()
        {
            if (m_scene != nullptr)
                return true;
            if (m_scenes == nullptr)
                return false;
            m_scenes->RegisterManager(&m_sceneManager);
            m_scene = m_sceneManager.CreateScene(u8"model.thumbnails");
            m_scene->SetSimulationEnabled(false); // so the animators hold the rest pose
            if (auto* environment = m_scene->GetSystem<engine::render::EnvironmentSystem>())
            {
                environment->Environment().skyMode = render::SkyMode::Procedural;
                environment->Environment().ambientIntensity = 0.3f;
            }
            if (auto* lights = m_scene->GetSystem<engine::render::LightComponentManager>())
            {
                const scene::EntityHandle sun = m_scene->CreateEntity(u8"sun");
                Transform t;
                t.rotation = FromYawPitchRoll(0.6f, -0.9f, 0.0f);
                m_scene->SetLocalTransform(sun, t);
                engine::render::LightComponent& light = lights->Add(sun);
                light.type = engine::render::LightType::Directional;
                light.intensity = 2.5f;
            }
            return true;
        }

        bool EnsureTarget()
        {
            if (m_target != nullptr)
                return true;
            if (m_device == nullptr)
                return false;
            rhi::TextureDesc desc;
            desc.width = m_size;
            desc.height = m_size;
            desc.format = rhi::TextureFormat::RGBA16Float;
            desc.usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::CopySrc;
            if (!m_device->CreateTexture(desc, m_target).IsOk() || m_target == nullptr)
            {
                m_target = nullptr;
                return false;
            }
            if (!m_device->CreateTextureView(m_target, rhi::TextureViewDesc{}, m_targetView).IsOk() || m_targetView == nullptr)
            {
                m_device->DestroyTexture(m_target);
                m_target = nullptr;
                m_targetView = nullptr;
                return false;
            }
            m_targetState = rhi::ResourceState::Undefined;
            return true;
        }

        IAllocator* m_allocator;
        engine::scene::SceneSubsystem* m_scenes = nullptr;
        engine::render::RenderSubsystem* m_render = nullptr;
        rhi::Device* m_device = nullptr;
        scene::SceneManager m_sceneManager;
        scene::Scene* m_scene = nullptr;
        rhi::Texture* m_target = nullptr;
        rhi::TextureView* m_targetView = nullptr;
        rhi::ResourceState m_targetState = rhi::ResourceState::Undefined;
        foundation::rhi::ScreenshotCapture m_capture;

        Array<String> m_queue;
        usize m_next = 0;
        String m_cacheRoot;
        String m_thumbRoot;
        u32 m_size = 128;
        Function<void(StringView, StringView)> m_onDone;

        State m_state = State::Idle;
        String m_model;
        String m_png;
        UniquePtr<OpenModel> m_open;
        engine::model::ModelInstance m_instance;
        Float3 m_center{};
        f32 m_radius = 1.0f;
        u32 m_settleFrames = 0;
        bool m_looseTextures = true;
        FileLoads m_loads; // declared last so it is destroyed first, stopping its worker before the other members go
    };
}
