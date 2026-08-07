// Draconic::EditorScene - :animation_clip_page partition (implementation).

module;
#include <cmath> // std::fmod (looping playhead wrap)
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Log/Log.h"

module draconic.editor.scene;

import draconic.foundation;
import draconic.content;
import draconic.rhi;
import draconic.graphics;
import draconic.shell;
import draconic.runtime;
import draconic.runtime.client;
import draconic.scene;
import draconic.engine.scene;
import draconic.animation;
import draconic.animation.resource;
import draconic.animation.editor;
import draconic.resource;
import draconic.render;
import draconic.engine.render;
import draconic.ui;
import draconic.ui.toolkit;
import draconic.ui.runtime;
import draconic.ui.viewport;
import draconic.vg.renderer;
import draconic.editor.core;
import draconic.editor.app;
import :camera;

using namespace draconic::foundation;

namespace draconic::editor
{
    // ============================ Construction ==============================================

    AnimationClipEditorPage::AnimationClipEditorPage(EditorContext& context,
                                                     runtime::IApplicationHost& host,
                                                     ui::runtime::UIHost& uiHost,
                                                     draconic::content::Instance& instance)
        : m_context(&context), m_host(&host), m_uiHost(&uiHost), m_title(instance.Name())
    {
        m_router =
            MakeUnique<draconic::shell::InputRouter>(DefaultAllocator(), host.Shell()->Input());
        m_camera.position = Float3{0.0f, 1.4f, 3.2f};
        m_camera.LookAt(Float3{0.0f, 0.9f, 0.0f});
        m_scenes = host.Ctx().GetSubsystem<scene::SceneSubsystem>();
        m_render = host.Ctx().GetSubsystem<render::RenderSubsystem>();

        SetInstanceId(instance.Id());

        RefPtr<ISerializable> object = instance.ReadObject();
        m_asset = RefPtr<animation::AnimationClipAsset>(
            Cast<animation::AnimationClipAsset>(object.Get()));
        if (m_asset.Get() == nullptr)
        {
            DRACONIC_LOG_ERROR(u8"Editor",
                               u8"animation clip '{}' failed to read - page opens empty", m_title);
        }
        m_undoBaseline = SnapshotAsset();
        if (m_context->Resources() != nullptr)
        {
            m_clip = m_context->Resources()->Bind<animation::AnimationClip>(InstanceId());
        }

        BuildPreviewScene();
        m_viewport = MakeRef<ui::viewport::ViewportView>(DefaultAllocator());
        m_viewport->ClearColor = rhi::ClearColor{0.05f, 0.05f, 0.07f, 1.0f};

        // Transport: skeleton pick + play/pause + a normalized scrub slider + time readout.
        auto transport = MakeRef<ui::FlexLayout>(DefaultAllocator());
        transport->Direction = ui::Orientation::Horizontal;
        transport->Spacing = 6.0f;
        transport->Padding = ui::Thickness{6, 4};
        {
            AnimationClipEditorPage* self = this;
            m_skeletonButton =
                MakeRef<ui::Button>(DefaultAllocator(), StringView(u8"Skeleton: (none)"));
            m_skeletonButton->OnClick.Add([self](ui::ButtonBase*) { self->PickPreviewSkeleton(); });
            transport->AddView(m_skeletonButton.Get());
            m_playButton = MakeRef<ui::Button>(DefaultAllocator(), StringView(u8"Pause"));
            m_playButton->OnClick.Add(
                [self](ui::ButtonBase*)
                {
                    self->m_playing = !self->m_playing;
                    self->m_playButton->SetText(self->m_playing ? StringView(u8"Pause")
                                                                : StringView(u8"Play"));
                });
            transport->AddView(m_playButton.Get());

            m_timeSlider = MakeRef<ui::Slider>(DefaultAllocator());
            m_timeSlider->Min.SetValue(0.0f);
            m_timeSlider->Max.SetValue(1.0f);
            m_timeSlider->OnValueChanged.Add(
                [self](ui::Slider*, f32 v)
                {
                    if (self->m_scrubbing)
                    {
                        return; // playback echo
                    }
                    animation::AnimationClip* clip = self->m_clip.Get();
                    if (clip != nullptr && clip->duration > 0.0f)
                    {
                        self->m_time = v * clip->duration;
                        self->m_playing = false;
                        self->m_playButton->SetText(u8"Play");
                    }
                });
            auto slp = MakeRef<ui::FlexLayoutParams>(DefaultAllocator());
            slp->Grow = 1.0f;
            transport->AddView(m_timeSlider.Get(), slp);

            m_timeLabel = MakeRef<ui::Label>(DefaultAllocator());
            m_timeLabel->FontSize.SetValue(Optional<f32>{12.0f});
            m_timeLabel->VAlign.SetValue(fonts::VerticalAlignment::Middle);
            auto tlp = MakeRef<ui::FlexLayoutParams>(DefaultAllocator());
            tlp->Width = ui::SizeSpec::Fixed(ui::Unit::Px(110.0f));
            transport->AddView(m_timeLabel.Get(), tlp);
        }

        auto previewColumn = MakeRef<ui::FlexLayout>(DefaultAllocator());
        previewColumn->Direction = ui::Orientation::Vertical;
        {
            auto lp = MakeRef<ui::FlexLayoutParams>(DefaultAllocator());
            lp->Width = ui::SizeSpec::Match();
            previewColumn->AddView(transport.Get(), lp);
            auto grow = MakeRef<ui::FlexLayoutParams>(DefaultAllocator());
            grow->Grow = 1.0f;
            grow->Width = ui::SizeSpec::Match();
            previewColumn->AddView(m_viewport.Get(), grow);
        }

        m_grid = MakeRef<ui::toolkit::PropertyGrid>(DefaultAllocator());
        RebuildGrid();

        auto split = MakeRef<ui::toolkit::SplitView>(DefaultAllocator());
        split->SetSplitRatio(0.66f);
        split->SetPanes(previewColumn.Get(), m_grid.Get());
        m_content = split;
    }

    // ============================ Preview ===================================================

    void AnimationClipEditorPage::BuildPreviewScene()
    {
        if (m_scenes == nullptr)
        {
            return;
        }
        m_sceneManager.SetAwareRegistry(&m_scenes->AwareRegistry());
        m_scenes->RegisterManager(&m_sceneManager);
        m_scene = m_sceneManager.CreateScene(u8"animclip.preview");
    }

    void AnimationClipEditorPage::PickPreviewSkeleton()
    {
        ui::UIContext* ctx = Ctx();
        if (ctx == nullptr || m_context->Project() == nullptr)
        {
            return;
        }
        AnimationClipEditorPage* self = this;
        Array<String> types;
        types.PushBack(String(u8"SkeletonAsset"));
        auto dialog = MakeRef<app::AssetPickerDialog>(DefaultAllocator(), *m_context, Move(types));
        dialog->OnPicked = [self](const Guid& picked)
        {
            self->m_skeletonGuid = picked;
            if (self->m_context->Resources() != nullptr && !picked.IsNil())
            {
                self->m_skeleton = self->m_context->Resources()->Bind<animation::Skeleton>(picked);
            }
            else
            {
                self->m_skeleton = draconic::resource::Proxy<animation::Skeleton>{};
            }
            String label(u8"Skeleton: ");
            if (self->m_context->Project() != nullptr && !picked.IsNil())
            {
                if (draconic::content::Instance* inst =
                        self->m_context->Project()->SourceDb().GetInstance(picked))
                {
                    label.Append(inst->Name());
                }
                else
                {
                    label.Append(u8"(missing)");
                }
            }
            else
            {
                label.Append(u8"(none)");
            }
            self->m_skeletonButton->SetText(label.AsView());
        };
        dialog->Show(ctx);
    }

    void AnimationClipEditorPage::UpdatePreview(f32 dt)
    {
        animation::AnimationClip* clip = m_clip.Get();
        animation::Skeleton* skeleton = m_skeleton.Get();
        if (clip == nullptr || clip->duration <= 0.0f)
        {
            if (m_timeLabel.Get() != nullptr)
            {
                m_timeLabel->SetText(u8"(clip not cooked)");
            }
            return;
        }

        if (m_playing)
        {
            m_time += dt;
            if (m_time > clip->duration)
            {
                m_time = clip->isLooping ? std::fmod(m_time, clip->duration) : clip->duration;
                if (!clip->isLooping)
                {
                    m_playing = false;
                    m_playButton->SetText(u8"Play");
                }
            }
            // Echo playback into the slider without re-entering the scrub handler.
            m_scrubbing = true;
            m_timeSlider->Value.SetValue(m_time / clip->duration);
            m_scrubbing = false;
        }
        if (m_timeLabel.Get() != nullptr)
        {
            m_timeLabel->SetText(Format(u8"{}s / {}s", static_cast<i32>(m_time * 100.0f) / 100.0f,
                                        static_cast<i32>(clip->duration * 100.0f) / 100.0f)
                                     .AsView());
        }

        if (skeleton == nullptr || skeleton->BoneCount() <= 0 || m_render == nullptr ||
            !m_render->IsReady() || m_scene == nullptr)
        {
            return;
        }
        const usize boneCount = static_cast<usize>(skeleton->BoneCount());
        m_poseScratch.Resize(boneCount);
        animation::SampleClip(*clip, *skeleton, m_time,
                              Span<animation::BoneTransform>{m_poseScratch.Data(), boneCount});
        auto& draw = m_render->DebugScene(*m_scene);
        draw.DrawGrid(Float3{0.0f, 0.0f, 0.0f}, 4.0f, 8, Color{0.25f, 0.25f, 0.28f, 1.0f});
        DrawSkeletonWireframe(draw, *skeleton,
                              Span<const animation::BoneTransform>{m_poseScratch.Data(), boneCount},
                              m_worldScratch);
    }

    // ============================ Inspector =================================================

    void AnimationClipEditorPage::RebuildGrid()
    {
        m_grid->Clear();
        if (m_asset.Get() == nullptr)
        {
            return;
        }
        AnimationClipEditorPage* self = this;
        animation::AnimationClipSource& source = m_asset->source;
        ui::toolkit::PropertyGrid& g = *m_grid;

        // --- stats (read-only) ---
        {
            const StringView cat = u8"Clip";
            auto stat = [&](StringView name, String value)
            {
                g.AddProperty(RefPtr<ui::toolkit::PropertyEditor>(
                    MakeRef<ui::toolkit::StringEditor>(DefaultAllocator(), name, value.AsView(),
                                                       Function<void(StringView)>{}, cat)
                        .Get()));
            };
            stat(u8"Name", String(source.name.AsView()));
            stat(u8"Duration", Format(u8"{} s", source.duration));
            stat(u8"Tracks", Format(u8"{}", source.trackBone.Size()));

            g.AddProperty(RefPtr<ui::toolkit::PropertyEditor>(
                MakeRef<ui::toolkit::BoolEditor>(DefaultAllocator(), u8"Looping", source.isLooping,
                                                 Function<void(bool)>{[self, &source](bool v)
                                                                      {
                                                                          source.isLooping = v;
                                                                          self->CommitEdit(
                                                                              u8"clip-loop");
                                                                      }},
                                                 cat)
                    .Get()));
        }

        // --- events (time + name, add/remove, undoable) ---
        while (source.eventNames.Size() < source.eventTimes.Size())
        {
            source.eventNames.PushBack(String{});
        }
        while (source.eventTimes.Size() < source.eventNames.Size())
        {
            source.eventTimes.PushBack(0.0f);
        }
        for (usize e = 0; e < source.eventTimes.Size(); ++e)
        {
            const String cat = Format(u8"Event {}", e);
            g.AddProperty(RefPtr<ui::toolkit::PropertyEditor>(
                MakeRef<ui::toolkit::FloatEditor>(
                    DefaultAllocator(), u8"Time (s)", static_cast<f64>(source.eventTimes[e]), 0.0,
                    static_cast<f64>(Max(source.duration, 0.0f)), 0.01, 3,
                    Function<void(f64)>{[self, &source, e](f64 v)
                                        {
                                            source.eventTimes[e] = static_cast<f32>(v);
                                            self->CommitEdit(u8"event-time");
                                        }},
                    cat.AsView())
                    .Get()));
            g.AddProperty(RefPtr<ui::toolkit::PropertyEditor>(
                MakeRef<ui::toolkit::StringEditor>(
                    DefaultAllocator(), u8"Name", source.eventNames[e].AsView(),
                    Function<void(StringView)>{[self, &source, e](StringView v)
                                               {
                                                   source.eventNames[e] = String(v);
                                                   self->CommitEdit(u8"event-name");
                                               }},
                    cat.AsView())
                    .Get()));
            const usize eventIdx = e;
            g.AddProperty(RefPtr<ui::toolkit::PropertyEditor>(
                MakeRef<ui::toolkit::ButtonEditor>(
                    DefaultAllocator(), u8"Remove Event",
                    Function<void()>{[self, eventIdx]()
                                     {
                                         self->QueueStructural(
                                             u8"del-event",
                                             Function<void()>{
                                                 [self, eventIdx]()
                                                 {
                                                     animation::AnimationClipSource& s =
                                                         self->m_asset->source;
                                                     if (eventIdx < s.eventTimes.Size())
                                                     {
                                                         s.eventTimes.RemoveAt(eventIdx);
                                                     }
                                                     if (eventIdx < s.eventNames.Size())
                                                     {
                                                         s.eventNames.RemoveAt(eventIdx);
                                                     }
                                                 }});
                                     }},
                    cat.AsView())
                    .Get()));
        }
        g.AddProperty(RefPtr<ui::toolkit::PropertyEditor>(
            MakeRef<ui::toolkit::ButtonEditor>(
                DefaultAllocator(), u8"+ Add Event",
                Function<void()>{[self]()
                                 {
                                     self->QueueStructural(
                                         u8"add-event",
                                         Function<void()>{[self]()
                                                          {
                                                              animation::AnimationClipSource& s =
                                                                  self->m_asset->source;
                                                              s.eventTimes.PushBack(
                                                                  self->m_time); // at the playhead
                                                              s.eventNames.PushBack(
                                                                  String(u8"event"));
                                                          }});
                                 }},
                StringView(u8"Events"))
                .Get()));
    }

    void AnimationClipEditorPage::QueueStructural(StringView undoKey, Function<void()> mutate)
    {
        AnimationClipEditorPage* self = this;
        String key(undoKey);
        auto run = [self, mutate = Move(mutate), key = Move(key)]() mutable
        {
            mutate();
            Array<byte> after = self->SnapshotAsset();
            (void)self->Commands().Execute(
                UniquePtr<IEditorCommand>(DefaultAllocator().New<EditClipCommand>(
                                              *self, key.AsView(), self->m_undoBaseline, after),
                                          DefaultAllocator()));
            self->m_undoBaseline = Move(after);
            self->RebuildGrid();
            self->MarkDirty();
        };
        if (ui::UIContext* ctx = Ctx())
        {
            ctx->MutationQueueRef().QueueAction(Function<void()>{Move(run)});
        }
        else
        {
            run();
        }
    }

    // ============================ Undo ======================================================

    Array<byte> AnimationClipEditorPage::SnapshotAsset() const
    {
        Array<byte> blob;
        if (m_asset.Get() == nullptr)
        {
            return blob;
        }
        MemoryStream stream;
        BinarySerializer ar(stream, SerializeMode::Write);
        m_asset->Serialize(ar);
        const Span<const byte> bytes = stream.Bytes();
        blob.Reserve(bytes.Size());
        for (byte b : bytes)
        {
            blob.PushBack(b);
        }
        return blob;
    }

    void AnimationClipEditorPage::ApplyAssetBlob(const Array<byte>& blob)
    {
        if (m_asset.Get() == nullptr)
        {
            return;
        }
        Array<byte> current = SnapshotAsset();
        if (current.Size() == blob.Size())
        {
            bool same = true;
            for (usize i = 0; i < blob.Size(); ++i)
            {
                if (current[i] != blob[i])
                {
                    same = false;
                    break;
                }
            }
            if (same)
            {
                return;
            }
        }
        MemoryStream stream;
        (void)stream.Write(blob.Data(), blob.Size());
        (void)stream.Seek(0, SeekOrigin::Begin);
        BinarySerializer ar(stream, SerializeMode::Read);
        // Clear the array fields in place (the source is an ISerializable - no copy assign).
        animation::AnimationClipSource& source = m_asset->source;
        source.trackBone.Clear();
        source.trackKind.Clear();
        source.trackInterp.Clear();
        source.trackStart.Clear();
        source.trackCount.Clear();
        source.eventTimes.Clear();
        source.eventNames.Clear();
        m_asset->Serialize(ar);
        m_undoBaseline = blob;
        AnimationClipEditorPage* self = this;
        if (ui::UIContext* ctx = Ctx())
        {
            ctx->MutationQueueRef().QueueAction(
                Function<void()>{[self]() { self->RebuildGrid(); }});
        }
        else
        {
            RebuildGrid();
        }
        MarkDirty();
    }

    void AnimationClipEditorPage::CommitEdit(StringView mergeKey)
    {
        Array<byte> after = SnapshotAsset();
        (void)Commands().Execute(UniquePtr<IEditorCommand>(
            DefaultAllocator().New<EditClipCommand>(*this, mergeKey, m_undoBaseline, after),
            DefaultAllocator()));
        m_undoBaseline = Move(after);
        MarkDirty();
    }

    // ============================ Frame / save / close ======================================

    ui::UIContext* AnimationClipEditorPage::Ctx() const
    {
        return (m_grid.Get() != nullptr) ? m_grid->Context : nullptr;
    }

    void AnimationClipEditorPage::OnUpdate(runtime::IApplicationHost&, f32 dt)
    {
        UpdatePreview(dt);
        EnsureViewportBound();
        if (m_hostWindow == nullptr)
        {
            return;
        }
        m_viewport->SyncInputRegion();
        if (m_router)
        {
            m_router->Update();
        }
        if (m_viewport->IsHovered() || m_viewport->IsFocused())
        {
            m_camera.Update(m_viewport->Keyboard(), m_viewport->Mouse(), dt);
        }
    }

    void AnimationClipEditorPage::OnRenderWindow(runtime::IApplicationHost&,
                                                 draconic::graphics::FrameContext& frame)
    {
        if (!m_viewport->IsReady() || !frame.valid)
        {
            return;
        }
        if (m_render == nullptr || !m_render->IsReady() || m_scene == nullptr)
        {
            return;
        }
        const u32 w = m_viewport->RenderWidth();
        const u32 h = m_viewport->RenderHeight();
        if (w == 0 || h == 0 || !m_viewport->IsEffectivelyVisible())
        {
            return;
        }

        render::ViewCamera camera;
        camera.view = Float4x4::LookAtRH(m_camera.position, m_camera.position + m_camera.Forward(),
                                         m_camera.Up());
        camera.projection = Float4x4::PerspectiveFovRH(
            1.0472f, static_cast<f32>(w) / static_cast<f32>(h), 0.05f, 200.0f);
        camera.position = m_camera.position;
        camera.farZ = 200.0f;

        render::CameraOverride cameraOverride;
        cameraOverride.camera = camera;
        cameraOverride.clearColor = Color{m_viewport->ClearColor.r, m_viewport->ClearColor.g,
                                          m_viewport->ClearColor.b, m_viewport->ClearColor.a};

        render::TargetState targetState;
        targetState.texture = m_viewport->ColorTexture();
        targetState.currentState = m_viewport->ColorState();
        targetState.finalState = rhi::ResourceState::ShaderRead;

        m_render->RenderScene(*m_scene, m_viewport->ColorTargetView(), m_viewport->ColorFormat(), w,
                              h, render::ViewportRect{0, 0, w, h}, &cameraOverride, targetState);
        m_viewport->SetColorState(rhi::ResourceState::ShaderRead);
    }

    void AnimationClipEditorPage::EnsureViewportBound()
    {
        draconic::ui::RootView* root = m_viewport->Root();
        if (root == nullptr)
        {
            return;
        }
        draconic::graphics::RenderWindow* window = m_uiHost->WindowForRoot(root);
        if (window == nullptr || window == m_hostWindow)
        {
            return;
        }
        vg::renderer::VGRenderer* renderer = m_uiHost->RendererFor(window);
        if (renderer == nullptr)
        {
            return;
        }
        if (m_hostWindow == nullptr)
        {
            m_viewport->Initialize(m_host->Graphics()->Raw(), renderer, m_host->Shell()->Input(),
                                   window->Window().Id());
            if (m_viewport->Surface() != nullptr)
            {
                m_router->AddSurface(m_viewport->Surface());
            }
        }
        else
        {
            m_viewport->AttachToWindow(renderer, window->Window().Id());
        }
        m_hostWindow = window;
    }

    Status AnimationClipEditorPage::Save()
    {
        if (m_asset.Get() == nullptr || m_context->Project() == nullptr)
        {
            return Status{ErrorCode::NotFound};
        }
        draconic::content::Instance* instance =
            m_context->Project()->SourceDb().GetInstance(InstanceId());
        if (instance == nullptr)
        {
            return Status{ErrorCode::NotFound};
        }
        const Status saved = instance->WriteObject(*m_asset);
        if (saved.IsOk())
        {
            ClearDirty();
            m_context->RequestCook(false);
            DRACONIC_LOG_INFO(u8"Editor", u8"saved animation clip '{}'", m_title);
        }
        return saved;
    }

    void AnimationClipEditorPage::OnClose()
    {
        if (m_viewport.Get() != nullptr)
        {
            m_viewport->Shutdown();
        }
        if (m_scene != nullptr)
        {
            m_sceneManager.DestroyScene(m_scene);
            m_scene = nullptr;
        }
        if (m_scenes != nullptr)
        {
            m_scenes->UnregisterManager(&m_sceneManager);
        }
    }

    // ============================ Factory ===================================================

    const TypeInfo* AnimationClipPageFactory::PrimaryType() const
    {
        return &animation::AnimationClipAsset::StaticType();
    }

    UniquePtr<EditorPage>
    AnimationClipPageFactory::CreatePage(EditorContext& context,
                                         draconic::content::Instance& instance)
    {
        auto* page =
            DefaultAllocator().New<AnimationClipEditorPage>(context, *m_host, *m_uiHost, instance);
        return UniquePtr<EditorPage>(page, DefaultAllocator());
    }

    void RegisterAnimationClipEditor(EditorContext& context, runtime::IApplicationHost& host,
                                     ui::runtime::UIHost& uiHost)
    {
        context.Pages().Register(UniquePtr<IEditorPageFactory>(
            DefaultAllocator().New<AnimationClipPageFactory>(host, uiHost), DefaultAllocator()));
    }
}
