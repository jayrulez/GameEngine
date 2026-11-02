// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Geometry - :model_file_preview partition, its body. See ModelFilePreview.cppm.

module;
#include "Core/Prelude.h"

module editor.geometry;

import foundation.core;
import foundation.graphics;
import foundation.runtime.client;
import foundation.rhi;
import foundation.scene;
import engine.scene;
import foundation.render;
import engine.render;
import foundation.geometry;
import foundation.model;
import foundation.model.resource;
import foundation.animation;
import engine.model;
import foundation.gui;
import foundation.gui.toolkit;
import pipeline.importer; // FileNameOf
import modelimporter;     // ModelManifestAsset, the type a model file is listed by
import editor.core;
import editor.app;
import editor.preview;
import editor.modelpreview;
import :model_file_preview;

using namespace foundation::core;

namespace editor
{
    namespace gui = foundation::gui;
    namespace scene = foundation::scene;
    namespace render = foundation::render;

    namespace
    {
        [[nodiscard]] RefPtr<gui::ToolButton> Tool(IAllocator& allocator, StringView icon, StringView tooltip)
        {
            RefPtr<gui::ToolButton> button = MakeRef<gui::ToolButton>(allocator);
            button->SetIconSource(String(icon));
            button->SetTooltip(tooltip);
            return button;
        }

        [[nodiscard]] bool Under(const scene::Scene& level, scene::EntityHandle entity, scene::EntityHandle root)
        {
            for (scene::EntityHandle e = entity; level.IsValid(e); e = level.GetParent(e))
                if (e == root)
                    return true;
            return false;
        }
    }

    struct ModelFilePreview::Impl
    {
        // What the worker reads, for the main thread to cook and bind.
        struct Read : RefCounted
        {
            foundation::model::Model model;
            ModelStats stats;
            OpenModel::Outcome outcome = OpenModel::Outcome::NotFound;
        };

        IAllocator* allocator;
        ModelFilePreview* owner;
        String path;
        foundation::rhi::Device* device = nullptr;
        RefPtr<gui::Node> root;
        gui::ComboBox* clips = nullptr;
        gui::ToolButton* play = nullptr;
        gui::ToolButton* skeleton = nullptr;
        gui::Label* message = nullptr;
        gui::Node* stage = nullptr;
        UniquePtr<PreviewViewport> viewport;
        OpenModel open;
        engine::model::ModelInstance instance;
        UniquePtr<ClipPlayback> playback;
        Array<String> facts;
        Array<String> clipNames;
        Array<Float4x4> bonePoses;
        Float3 center{};
        f32 radius = 1.0f;
        RefPtr<ImportPreviewRead> read;

        Impl(IAllocator& alloc, ModelFilePreview& preview, StringView file)
            : allocator(&alloc), owner(&preview), path(file), open(alloc), facts(alloc), clipNames(alloc), bonePoses(alloc)
        {
        }

        void BuildView(const app::AssetPageServices& services)
        {
            RefPtr<gui::Panel> panel = MakeRef<gui::Panel>(*allocator);
            panel->AddClass(u8"import-model");
            RefPtr<gui::Panel> bar = MakeRef<gui::Panel>(*allocator);
            bar->AddClass(u8"panel-toolbar");
            Impl* self = this;
            RefPtr<gui::ToolButton> frame = Tool(*allocator, u8"Editor/Icons/frame-selection.svg", u8"Frame it (F)");
            frame->SetOnClick([self]() { self->Frame(); });
            RefPtr<gui::ComboBox> clipChoice = MakeRef<gui::ComboBox>(*allocator);
            clipChoice->AddClass(u8"import-model-clip");
            clipChoice->SetTooltip(u8"The clip it plays");
            clipChoice->SetOnSelectionChanged([self](i32 index)
                                              {
                                                  if (self->playback && index >= 0)
                                                      self->playback->Select(index);
                                                  self->ShowPlay();
                                              });
            RefPtr<gui::ToolButton> playButton = Tool(*allocator, u8"Editor/Icons/pause.svg", u8"Play or pause the clip");
            playButton->SetOnClick([self]()
                                   {
                                       if (self->playback)
                                           self->playback->SetPlaying(!self->playback->Playing());
                                       self->ShowPlay();
                                   });
            RefPtr<gui::ToolButton> bones = Tool(*allocator, u8"Editor/Icons/character.svg", u8"Draw the bones over the model");
            bones->SetToggle(true);
            clips = clipChoice.Get();
            play = playButton.Get();
            skeleton = bones.Get();
            bar->AddChild(frame.Get());
            bar->AddChild(clipChoice.Get());
            bar->AddChild(playButton.Get());
            bar->AddChild(bones.Get());
            RefPtr<gui::Panel> stagePanel = MakeRef<gui::Panel>(*allocator);
            stagePanel->AddClass(u8"import-model-stage");
            stage = stagePanel.Get();
            RefPtr<gui::Label> text = MakeRef<gui::Label>(*allocator);
            text->AddClass(u8"state-text");
            text->AddClass(u8"import-model-message");
            message = text.Get();
            panel->AddChild(bar.Get());
            panel->AddChild(stagePanel.Get());
            panel->AddChild(text.Get());
            root = panel;
            ShowClips(); // none until it opens

            if (services.host == nullptr)
                return; // a test: no view, the model still opens
            if (services.host->Graphics() != nullptr)
                device = services.host->Graphics()->Raw();
            viewport = MakeUnique<PreviewViewport>(*allocator, *allocator, *services.host, services.gui, u8"import.model.preview");
            viewport->SetSimulationEnabled(true); // so its clip plays
            stage->AddChild(viewport->View().Get());
            Light();
        }

        // The model viewer's light: a sun from the side, down at about 50 degrees, over a sky.
        void Light()
        {
            scene::Scene* level = viewport ? viewport->Scene() : nullptr;
            if (level == nullptr)
                return;
            if (auto* environment = level->GetSystem<engine::render::EnvironmentSystem>())
            {
                engine::render::EnvironmentSettings& e = environment->Environment();
                e.skyMode = render::SkyMode::Procedural;
                e.skyIntensity = 1.0f;
                e.ambientIntensity = 0.3f;
            }
            if (auto* lights = level->GetSystem<engine::render::LightComponentManager>())
            {
                const scene::EntityHandle sun = level->CreateEntity(u8"sun");
                Transform t;
                t.rotation = FromYawPitchRoll(0.6f, -0.9f, 0.0f);
                level->SetLocalTransform(sun, t);
                engine::render::LightComponent& light = lights->Add(sun);
                light.type = engine::render::LightType::Directional;
                light.intensity = 2.5f;
                light.castsShadows = true;
            }
        }

        void Start(StringView cacheRoot, ImportPreviewReader& reader, foundation::model::Model* imported)
        {
            message->SetText(Format(u8"Reading {}...", pipeline::FileNameOf(path.AsView())).AsView());
            // As the importer reads it: without the viewer's loose texture rule, which the import
            // does not apply either, so the preview shows what will come in.
            open.SetLooseTextures(false);
            OpenModel::Outcome outcome = OpenModel::Outcome::NotFound;
            const OpenModel::Begun begun = open.Begin(path.AsView(), cacheRoot, device, outcome);
            if (begun == OpenModel::Begun::Bound)
            {
                Opened();
                return;
            }
            if (begun == OpenModel::Begun::Failed)
            {
                Failed(outcome);
                return;
            }
            // The importer read it already: cook and bind that, with no second read.
            if (imported != nullptr)
            {
                const OpenModel::Outcome finished = open.Finish(*imported, CountModel(*imported)); // CountModel works out its bounds, nothing more
                if (finished == OpenModel::Outcome::Opened)
                    Opened();
                else
                    Failed(finished);
                return;
            }
            RefPtr<Read> result = MakeRef<Read>(*allocator);
            Impl* self = this;
            // On the worker: only the file and `result` (LoadFile makes its own loaders).
            read = reader.Read([result, file = path]()
                               {
                                   result->outcome = OpenModel::LoadFile(file.AsView(), false, result->model, result->stats);
                               },
                               // On the main thread, which owns the cache's database and resources, and
                               // only while this preview lives (it cancels the read as it goes).
                               [self, result]()
                               {
                                   if (result->outcome != OpenModel::Outcome::Opened)
                                   {
                                       self->Failed(result->outcome);
                                       return;
                                   }
                                   const OpenModel::Outcome finished = self->open.Finish(result->model, result->stats);
                                   if (finished == OpenModel::Outcome::Opened)
                                       self->Opened();
                                   else
                                       self->Failed(finished);
                               });
        }

        void Failed(OpenModel::Outcome outcome)
        {
            const StringView why = outcome == OpenModel::Outcome::LoadFailed ? StringView(u8"The model loaders could not read this file.")
                                   : outcome == OpenModel::Outcome::NotFound ? StringView(u8"This file is missing.")
                                                                             : StringView(u8"This model did not cook for the preview; the Console says why.");
            message->SetText(why);
            message->SetDisplayed(true);
            if (owner->onChanged)
                owner->onChanged();
        }

        void Opened()
        {
            const ModelStats& stats = open.Stats();
            facts.PushBack(Format(u8"Meshes {}", stats.meshes));
            facts.PushBack(Format(u8"Triangles {}", stats.triangles));
            facts.PushBack(Format(u8"Materials {}", stats.materials));
            facts.PushBack(Format(u8"Textures {}", stats.textures));
            if (stats.bones > 0)
                facts.PushBack(Format(u8"Bones {}", stats.bones));
            if (!stats.clips.IsEmpty())
                facts.PushBack(Format(u8"Clips {}", stats.clips.Size()));
            for (const String& clip : stats.clips)
            {
                clipNames.PushBack(clip);
                clips->AddItem(clip.AsView());
            }
            message->SetText(StringView{});
            message->SetDisplayed(false);
            Place();
            ShowClips();
            if (owner->onChanged)
                owner->onChanged();
        }

        // Puts the bound model in the view, playing its first clip, framed.
        void Place()
        {
            scene::Scene* level = viewport ? viewport->Scene() : nullptr;
            if (level == nullptr || open.Model() == nullptr)
                return;
            instance = engine::model::InstantiateModel(*level, *open.Model(), pipeline::FileNameOf(path.AsView()));
            playback = MakeUnique<ClipPlayback>(*allocator, *level, instance.root, *open.Model());
            playback->SetLooping(true);
            MeasureBounds(*level);
            Frame();
            viewport->Camera().Advance(1000.0f); // there at once, with no glide on opening
        }

        void MeasureBounds(scene::Scene& level)
        {
            center = Float3{};
            radius = 1.0f;
            auto* meshes = level.GetSystem<engine::render::MeshComponentManager>();
            if (meshes == nullptr || !instance.root.IsAssigned())
                return;
            AABB box = AABB::Empty();
            meshes->ForEach([&](engine::render::MeshComponent& component, scene::EntityHandle entity)
                            {
                                const foundation::geometry::StaticMesh* mesh = component.mesh.Get();
                                if (mesh == nullptr || !mesh->bounds.IsValid() || !Under(level, entity, instance.root))
                                    return;
                                box = Merge(box, TransformAABB(mesh->bounds, level.ComposeWorldMatrix(entity)));
                            });
            if (!box.IsValid())
                return;
            center = (box.min + box.max) * 0.5f;
            radius = Max(Length(box.max - box.min) * 0.5f, 0.01f);
        }

        // The model viewer's framing: the clip range follows the model, so a small prop and a
        // large building are both in reach.
        void Frame()
        {
            if (!viewport)
                return;
            EditorCamera& camera = viewport->Camera();
            camera.nearZ = Max(radius * 0.05f, 0.0001f);
            camera.farZ = Max(radius * 100.0f, 50.0f);
            viewport->Frame(center, radius);
        }

        void ShowClips()
        {
            const bool any = !clipNames.IsEmpty() && playback && playback->HasClips();
            if (any)
                clips->SetSelectedIndex(Max(playback->Selected(), 0));
            clips->SetDisplayed(any);
            play->SetDisplayed(any);
            skeleton->SetDisplayed(!instance.skinned.IsEmpty());
            ShowPlay();
        }

        void ShowPlay()
        {
            const bool playing = playback && playback->Playing();
            play->SetIconSource(String(playing ? StringView(u8"Editor/Icons/pause.svg") : StringView(u8"Editor/Icons/play.svg")));
        }

        // The model viewer's skeleton: a line from each bone to its parent, in the current pose.
        void DrawSkeleton()
        {
            if (!skeleton->IsChecked() || !viewport || !viewport->IsValid() || instance.skinned.IsEmpty() || open.Model() == nullptr)
                return;
            const foundation::animation::AnimationPlayer* player = playback ? playback->Player() : nullptr;
            foundation::animation::Skeleton* bones = open.Model()->skeleton.Get();
            if (bones == nullptr)
                return;
            const usize count = static_cast<usize>(bones->BoneCount());
            bonePoses.Resize(count);
            bones->ComputeWorldPoses(player != nullptr ? player->GetFinalPoses() : Span<const foundation::animation::BoneTransform>{},
                                     Span<Float4x4>(bonePoses.Data(), count));
            const Float4x4 model = viewport->Scene()->ComposeWorldMatrix(instance.skinned[0]);
            const Color color{0.35f, 0.85f, 1.0f, 1.0f};
            render::debug::DebugDraw& draw = viewport->SceneDebugDraw();
            for (usize i = 0; i < count; ++i)
            {
                const i32 parent = bones->GetBone(static_cast<i32>(i))->parentIndex;
                if (parent < 0 || static_cast<usize>(parent) >= count)
                    continue;
                const Float4x4& a = bonePoses[static_cast<usize>(parent)];
                const Float4x4& b = bonePoses[i];
                draw.DrawLineOverlay(TransformPoint(Float3{a.m[3][0], a.m[3][1], a.m[3][2]}, model),
                                     TransformPoint(Float3{b.m[3][0], b.m[3][1], b.m[3][2]}, model), color);
            }
        }
    };

    ModelFilePreview::ModelFilePreview(StringView path, StringView cacheRoot, ImportPreviewReader& reader, IAllocator& allocator, const app::AssetPageServices& services,
                                       foundation::model::Model* imported)
    {
        m_impl = MakeUnique<Impl>(allocator, allocator, *this, path);
        Provide<app::IImportPreviewFrames>(*this);
        m_impl->BuildView(services);
        m_impl->Start(cacheRoot, reader, imported);
    }

    ModelFilePreview::~ModelFilePreview()
    {
        if (m_impl->read)
            m_impl->read->Cancel();
        // The frames in flight still draw the model's textures, so the GPU finishes them before
        // the model releases them.
        if (m_impl->device != nullptr)
            m_impl->device->WaitIdle();
        m_impl->playback.Reset();
        if (m_impl->viewport)
            m_impl->viewport->Shutdown();
        m_impl->viewport.Reset();
        m_impl->open.Close();
    }

    gui::Node& ModelFilePreview::View()
    {
        return *m_impl->root;
    }

    Span<const String> ModelFilePreview::Facts() const
    {
        return m_impl->facts.AsSpan();
    }

    void ModelFilePreview::OnUpdate(foundation::runtime::IApplicationHost&, f32 dt)
    {
        if (m_impl->open.Resources() != nullptr)
            m_impl->open.Resources()->Pump(); // the model's textures finish loading
        if (!m_impl->viewport)
            return;
        if (m_impl->viewport->Update(dt))
            m_impl->Frame(); // F over the view
        m_impl->DrawSkeleton();
        m_impl->ShowPlay();
    }

    void ModelFilePreview::OnRenderWindow(foundation::runtime::IApplicationHost&, foundation::graphics::FrameContext& frame)
    {
        if (m_impl->viewport)
            m_impl->viewport->RenderFrame(frame);
    }

    bool ModelFilePreview::IsOpen() const
    {
        return m_impl->open.IsOpen();
    }

    Span<const String> ModelFilePreview::Clips() const
    {
        return m_impl->clipNames.AsSpan();
    }

    UniquePtr<IImportPreview> ModelFilePreviewFactory::Create(const ImportPreviewSource& file, ImportPreviewReader& reader, IAllocator& allocator)
    {
        if (m_context->Project() == nullptr)
            return {};
        const String cacheRoot = PathJoin(m_context->Project()->CacheRoot().AsView(), StringView(kModelPreviewCacheFolder));
        const app::AssetPageServices services = m_services->Make();
        auto* loaded = Cast<pipeline::LoadedModel>(file.prepared);
        return UniquePtr<IImportPreview>(
            allocator.New<ModelFilePreview>(file.path, cacheRoot.AsView(), reader, allocator, services, loaded != nullptr ? &loaded->model : nullptr), allocator);
    }

    void RegisterModelFilePreview(EditorContext& context, const app::AssetPageServicesSource& services)
    {
        context.ImportPreviews().Register(UniquePtr<IImportPreviewFactory>(
            editor::EditorRootAllocator().New<ModelFilePreviewFactory>(pipeline::ModelManifestAsset::StaticType(), context, services), editor::EditorRootAllocator()));
    }
}
