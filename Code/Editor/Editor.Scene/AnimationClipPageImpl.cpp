// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Scene - :animation_clip_page partition (implementation).

module;
#include <cmath> // std::fmod (looping playhead wrap)
#include "Core/Prelude.h"
#include "Core/Log/Log.h"
#include "Core/Reflection/Reflect.h"

module editor.scene;

import foundation.core;
import foundation.settings; // per-project editor-settings store (preview skeleton/mesh prefs)
import foundation.content;
import foundation.rhi;
import foundation.graphics;
import foundation.shell;
import foundation.runtime;
import foundation.runtime.client;
import foundation.scene;
import engine.scene;
import foundation.animation;
import foundation.animation.resource;
import animation.pipeline;
import foundation.resource;
import foundation.render;
import engine.render;
import foundation.ui;
import foundation.ui.toolkit;
import foundation.ui.runtime;
import foundation.ui.viewport;
import foundation.vg.renderer;
import editor.core;
import editor.app;
import editor.preview;
import :animation_graph_page; // DrawSkeletonWireframe (shared preview helper, impl-only)

using namespace foundation::core;
namespace animation = foundation::animation;
namespace render = foundation::render;
namespace rhi = foundation::rhi;
namespace runtime = foundation::runtime;
namespace scene = foundation::scene; // :actions declares it too; MSVC does not see a partition's
namespace ui = foundation::ui;
namespace vg = foundation::vg;
namespace fonts = foundation::fonts;

namespace editor
{
    // Per-asset clip-preview prefs: {clipGuid -> (skeleton guid, skinned-mesh guid)} - a section in
    // the per-project editor-settings store, so a reopened clip viewer restores its preview rig.
    struct ClipPreviewPref
    {
        Guid asset;
        Guid skeleton;
        Guid mesh;
        void Serialize(ISerializer& ar)
        {
            ar.Key("asset");
            ar.GuidValue(asset);
            ar.Key("skeleton");
            ar.GuidValue(skeleton);
            ar.Key("mesh");
            ar.GuidValue(mesh);
        }
    };
    inline void Serialize(ISerializer& ar, ClipPreviewPref& p)
    {
        ar.BeginObject();
        p.Serialize(ar);
        ar.EndObject();
    }
    class ClipPreviewSettings final : public ISerializable
    {
        RTTI_OBJECT(ClipPreviewSettings, ISerializable)
    public:
        Array<ClipPreviewPref> prefs;
        void Serialize(ISerializer& ar) override
        {
            foundation::core::Serialize(ar, "prefs", prefs);
        }
    };

    // ============================ Construction ==============================================

    AnimationClipEditorPage::AnimationClipEditorPage(EditorContext& context,
                                                     runtime::IApplicationHost& host,
                                                     ui::runtime::UIHost& uiHost,
                                                     foundation::content::Instance& instance)
        : app::UIEditorPage(context.Allocator()),
          m_context(&context), m_host(&host), m_uiHost(&uiHost), m_title(instance.Name())
    {
        // Shared preview substrate (viewport + preview scene + orbit camera + render loop).
        m_preview =
            MakeUnique<PreviewViewport>(Allocator(), host, uiHost, u8"animclip.preview");
        m_preview->SetClearColor(Color{0.248f, 0.248f, 0.293f, 1.0f});
        m_preview->Camera().position = Float3{0.0f, 1.4f, 3.2f};
        m_preview->Camera().LookAt(Float3{0.0f, 0.9f, 0.0f});

        SetInstanceId(instance.Id());
        Provide<IPlaybackPage>(*this);

        RefPtr<ISerializable> object = instance.ReadObject();
        m_asset = RefPtr<pipeline::AnimationClipAsset>(
            Cast<pipeline::AnimationClipAsset>(object.Get()));
        if (m_asset.Get() == nullptr)
        {
            LOG_ERROR(u8"Editor",
                               u8"animation clip '{}' failed to read - page opens empty", m_title);
        }
        m_undoBaseline = SnapshotAsset();
        if (m_context->Resources() != nullptr)
        {
            m_clip = m_context->Resources()->Bind<animation::AnimationClip>(InstanceId());
        }

        BuildPreviewScene();

        // Transport: skeleton pick + play/pause + a normalized scrub slider + time readout.
        auto transport = MakeRef<ui::FlexLayout>(Allocator());
        transport->Direction = ui::Orientation::Horizontal;
        transport->Spacing = 6.0f;
        transport->Padding = ui::Thickness{6, 4};
        {
            AnimationClipEditorPage* self = this;
            // The preview rig: compact asset slots (pick, drop and clear are one assignment).
            const StringView skeletonTypes[] = {u8"SkeletonAsset"};
            m_skeletonSlot = MakeRef<app::CompactAssetSlot>(
                Allocator(), StringView(u8"Skeleton"), Span<const StringView>{skeletonTypes, 1});
            m_skeletonSlot->Editor().BindAsset(*m_context,
                                               [self]() { return self->m_skeletonGuid; },
                                               [self](const Guid& picked)
                                               {
                                                   self->SetPreviewSkeleton(picked);
                                                   self->SavePreviewPref();
                                               });
            m_skeletonSlot->Build();
            const StringView meshTypes[] = {u8"SkinnedMeshAsset"};
            m_meshSlot = MakeRef<app::CompactAssetSlot>(Allocator(), StringView(u8"Mesh"),
                                                        Span<const StringView>{meshTypes, 1});
            m_meshSlot->Editor().BindAsset(*m_context, [self]() { return self->m_previewMeshId; },
                                           [self](const Guid& picked)
                                           {
                                               self->SetPreviewMesh(picked);
                                               self->SavePreviewPref();
                                           });
            m_meshSlot->Build();
            {
                ui::LayoutStyle slot;
                slot.Width = ui::SizeSpec::Fixed(ui::Unit::Dp(200.0f));
                transport->AddView(m_skeletonSlot.Get(), slot);
                transport->AddView(m_meshSlot.Get(), slot);
            }

            m_timeSlider = MakeRef<ui::Slider>(Allocator());
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
                    }
                });
            ui::LayoutStyle slp;
            slp.FlexGrow = 1.0f;
            transport->AddView(m_timeSlider.Get(), slp);

            m_timeLabel = MakeRef<ui::Label>(Allocator());
            m_timeLabel->FontSize.SetValue(Optional<f32>{12.0f});
            m_timeLabel->VAlign.SetValue(fonts::VerticalAlignment::Middle);
            ui::LayoutStyle tlp;
            tlp.Width = ui::SizeSpec::Fixed(ui::Unit::Dp(110.0f));
            transport->AddView(m_timeLabel.Get(), tlp);
        }

        auto previewColumn = MakeRef<ui::FlexLayout>(Allocator());
        previewColumn->Direction = ui::Orientation::Vertical;
        {
            ui::LayoutStyle lp;
            lp.Width = ui::SizeSpec::Match();
            previewColumn->AddView(transport.Get(), lp);
            ui::LayoutStyle grow;
            grow.FlexGrow = 1.0f;
            grow.Width = ui::SizeSpec::Match();
            previewColumn->AddView(m_preview->View(), grow);
        }

        m_grid = MakeRef<ui::toolkit::PropertyGrid>(Allocator());
        RebuildGrid();

        auto split = MakeRef<ui::toolkit::SplitView>(Allocator());
        split->SetSplitRatio(0.66f);
        split->SetPanes(previewColumn.Get(), m_grid.Get());
        // The page toolbar (Save, Undo, Redo, Discard, then the playback transport) over the page.
        m_toolbar = MakeRef<app::PageToolbar>(Allocator(), *this, m_context->Actions());
        m_toolbar->AddPlayback();
        m_content = app::PageToolbar::Frame(Allocator(), *m_toolbar, *split);

        // Restore the persisted preview rig (skeleton + skinned mesh) for this clip.
        LoadPreviewPref();
    }

    void AnimationClipEditorPage::LoadPreviewPref()
    {
        foundation::settings::Settings* store = m_context->ProjectEditorSettings();
        if (store == nullptr)
        {
            return;
        }
        const ClipPreviewSettings* section = store->Find<ClipPreviewSettings>();
        if (section == nullptr)
        {
            return;
        }
        for (const ClipPreviewPref& p : section->prefs)
        {
            if (p.asset != InstanceId())
            {
                continue;
            }
            SetPreviewSkeleton(p.skeleton);
            SetPreviewMesh(p.mesh);
            return;
        }
    }

    void AnimationClipEditorPage::SavePreviewPref()
    {
        foundation::settings::Settings* store = m_context->ProjectEditorSettings();
        if (store == nullptr)
        {
            return;
        }
        ClipPreviewSettings& section = store->Section<ClipPreviewSettings>();
        for (ClipPreviewPref& p : section.prefs)
        {
            if (p.asset == InstanceId())
            {
                p.skeleton = m_skeletonGuid;
                p.mesh = m_previewMeshId;
                return;
            }
        }
        section.prefs.PushBack(ClipPreviewPref{InstanceId(), m_skeletonGuid, m_previewMeshId});
    }

    // ============================ Preview ===================================================

    void AnimationClipEditorPage::BuildPreviewScene()
    {
        scene::Scene* scenePtr = m_preview ? m_preview->Scene() : nullptr;
        if (scenePtr == nullptr)
        {
            return;
        }

        // A sun so a picked skinned mesh is lit (the skeleton wireframe needs none).
        const scene::EntityHandle sun = scenePtr->CreateEntity(u8"Sun");
        Transform st;
        st.rotation = Quaternion::FromAxisAngle(Float3{0, 1, 0}, 0.35f) *
                      Quaternion::FromAxisAngle(Float3{1, 0, 0}, -1.05f);
        scenePtr->SetLocalTransform(sun, st);
        if (auto* lights = scenePtr->GetSystem<engine::render::LightComponentManager>())
        {
            engine::render::LightComponent& light = lights->Add(sun);
            light.castsShadows = false;
        }

        // The optional skinned mesh: its MeshComponent gets bone matrices fed each frame from the
        // preview player (see UpdatePreview). No mesh bound until the user picks one.
        m_meshEntity = scenePtr->CreateEntity(u8"PreviewMesh");
        if (auto* meshes = scenePtr->GetSystem<engine::render::MeshComponentManager>())
        {
            meshes->Add(m_meshEntity);
        }
    }

    void AnimationClipEditorPage::SetPreviewSkeleton(const Guid& id)
    {
        m_skeletonGuid = id;
        if (m_context->Resources() != nullptr && !id.IsNil())
        {
            m_skeleton = m_context->Resources()->Bind<animation::Skeleton>(id);
        }
        else
        {
            m_skeleton = foundation::resource::Proxy<animation::Skeleton>{};
        }
        if (m_skeletonSlot.Get() != nullptr)
        {
            m_skeletonSlot->Editor().Refresh();
        }
    }

    void AnimationClipEditorPage::SetPreviewMesh(const Guid& id)
    {
        m_previewMeshId = id;
        if (m_context->Resources() != nullptr && !id.IsNil())
        {
            m_previewMesh = m_context->Resources()->Bind<foundation::geometry::StaticMesh>(id);
        }
        else
        {
            m_previewMesh = foundation::resource::Proxy<foundation::geometry::StaticMesh>{};
        }
        // Point the preview MeshComponent at the mesh (bone matrices feed per frame).
        scene::Scene* scenePtr = m_preview ? m_preview->Scene() : nullptr;
        if (scenePtr != nullptr)
        {
            if (auto* meshes = scenePtr->GetSystem<engine::render::MeshComponentManager>())
            {
                if (auto* mc = meshes->Get(m_meshEntity))
                {
                    if (foundation::geometry::StaticMesh* pm = m_previewMesh.Get())
                    {
                        mc->mesh = pm;
                    }
                    else
                    {
                        mc->mesh.SetDirect(RefPtr<foundation::geometry::StaticMesh>{});
                        mc->boneMatrices = nullptr;
                        mc->boneCount = 0;
                    }
                }
            }
        }
        if (m_meshSlot.Get() != nullptr)
        {
            m_meshSlot->Editor().Refresh();
        }
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
                }
            }
            // Echo playback into the slider without re-entering the scrub handler.
            SyncTimeSlider();
        }
        if (m_timeLabel.Get() != nullptr)
        {
            m_timeLabel->SetText(Format(u8"{}s / {}s", static_cast<i32>(m_time * 100.0f) / 100.0f,
                                        static_cast<i32>(clip->duration * 100.0f) / 100.0f)
                                     .AsView());
        }

        scene::Scene* scenePtr = m_preview ? m_preview->Scene() : nullptr;
        if (skeleton == nullptr || skeleton->BoneCount() <= 0 || m_preview.Get() == nullptr ||
            !m_preview->IsValid() || scenePtr == nullptr)
        {
            return;
        }
        const usize boneCount = static_cast<usize>(skeleton->BoneCount());
        m_poseScratch.Resize(boneCount);
        animation::SampleClip(*clip, *skeleton, m_time,
                              Span<animation::BoneTransform>{m_poseScratch.Data(), boneCount});

        // Root motion: the cooked pose plays in place; with Show travel the rig goes where the
        // clip's extracted travel takes it from its start (one loop's worth).
        Float4x4 travel = Float4x4::Identity();
        if (m_showTravel && !clip->rootMotion.IsEmpty())
        {
            const animation::RootMotionDelta moved = animation::ClipRootMotion(*clip, 0.0f, m_time, false);
            travel = RotationMatrix(Quaternion::FromAxisAngle(Float3{0.0f, 1.0f, 0.0f}, moved.yaw)) *
                     Float4x4::Translation(moved.translation);
        }
        if (auto* meshes = scenePtr->GetSystem<engine::render::MeshComponentManager>())
        {
            if (meshes->Get(m_meshEntity) != nullptr)
            {
                Transform at;
                at.position = Float3{travel.m[3][0], travel.m[3][1], travel.m[3][2]};
                at.rotation = QuaternionFromRotationMatrix(travel);
                scenePtr->SetLocalTransform(m_meshEntity, at);
            }
        }

        // Skinned preview mesh: drive the player to the SAME m_time and push its skinning matrices
        // onto the MeshComponent (borrowed for this frame's render).
        if (m_previewMesh.Get() != nullptr)
        {
            if (m_previewPlayer.Get() == nullptr || m_playerSkeleton != skeleton)
            {
                m_previewPlayer =
                    MakeUnique<animation::AnimationPlayer>(Allocator(), *skeleton);
                m_playerSkeleton = skeleton;
                m_playerClip = nullptr;
            }
            if (m_playerClip != clip)
            {
                m_playerClip = clip;
                m_previewPlayer->Play(clip);
            }
            m_previewPlayer->SetCurrentTime(m_time);
            m_previewPlayer->Update(0.0f); // resample at m_time without advancing
            const Span<const Float4x4> mats = m_previewPlayer->GetSkinningMatrices();
            if (auto* meshes = scenePtr->GetSystem<engine::render::MeshComponentManager>())
            {
                if (auto* mc = meshes->Get(m_meshEntity))
                {
                    mc->boneMatrices = mats.Data();
                    mc->boneCount = static_cast<u32>(mats.Size());
                }
            }
        }

        auto& draw = m_preview->SceneDebugDraw();
        draw.DrawGrid(Float3{0.0f, 0.0f, 0.0f}, 4.0f, 8, Color{0.25f, 0.25f, 0.28f, 1.0f});
        DrawSkeletonWireframe(draw, *skeleton,
                              Span<const animation::BoneTransform>{m_poseScratch.Data(), boneCount},
                              m_worldScratch, travel);

        // The extracted path on the ground (height too when the clip extracts it), and where the
        // clip is along it now.
        const animation::RootMotionCurve& curve = clip->rootMotion;
        if (!curve.IsEmpty())
        {
            const Color path{1.0f, 0.75f, 0.2f, 1.0f};
            const auto onGround = [&](Float3 p)
            {
                const Float3 start = curve.positions[0];
                return Float3{curve.horizontal ? p.x - start.x : 0.0f, curve.vertical ? p.y - start.y : 0.0f,
                              curve.horizontal ? p.z - start.z : 0.0f};
            };
            for (usize i = 0; i + 1 < curve.positions.Size(); ++i)
            {
                draw.DrawLine(onGround(curve.positions[i]), onGround(curve.positions[i + 1]), path, true);
            }
            const animation::RootMotionDelta now = animation::ClipRootMotion(*clip, 0.0f, m_time, false);
            draw.DrawWireSphere(now.translation, 0.04f, path, 10, true);
        }
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
                    MakeRef<ui::toolkit::StringEditor>(Allocator(), name, value.AsView(),
                                                       Function<void(StringView)>{}, cat)
                        .Get()));
            };
            stat(u8"Name", String(source.name.AsView()));
            stat(u8"Duration", Format(u8"{} s", source.duration));
            stat(u8"Tracks", Format(u8"{}", source.trackBone.Size()));

            g.AddProperty(RefPtr<ui::toolkit::PropertyEditor>(
                MakeRef<ui::toolkit::BoolEditor>(Allocator(), u8"Looping", source.isLooping,
                                                 Function<void(bool)>{[self, &source](bool v)
                                                                      {
                                                                          source.isLooping = v;
                                                                          self->CommitEdit(
                                                                              u8"clip-loop");
                                                                      }},
                                                 cat)
                    .Get()));
        }

        // --- root motion (root-motion.md): what the cook extracts; the preview shows its path ---
        {
            const StringView cat = u8"Root Motion";
            g.AddProperty(RefPtr<ui::toolkit::PropertyEditor>(
                MakeRef<ui::toolkit::StringEditor>(
                    Allocator(), u8"Root bone", source.rootMotion.rootBone.AsView(),
                    Function<void(StringView)>{[self, &source](StringView v)
                                               {
                                                   source.rootMotion.rootBone = String(v);
                                                   self->CommitEdit(u8"clip-rootbone");
                                               }},
                    cat)
                    .Get()));
            const auto toggle = [&](StringView name, bool& field)
            {
                bool* target = &field;
                g.AddProperty(RefPtr<ui::toolkit::PropertyEditor>(
                    MakeRef<ui::toolkit::BoolEditor>(Allocator(), name, field,
                                                     Function<void(bool)>{[self, target](bool v)
                                                                          {
                                                                              *target = v;
                                                                              self->CommitEdit(u8"clip-rootmotion");
                                                                          }},
                                                     cat)
                        .Get()));
            };
            toggle(u8"Horizontal", source.rootMotion.horizontal);
            toggle(u8"Vertical", source.rootMotion.vertical);
            toggle(u8"Yaw", source.rootMotion.yaw);
            g.AddProperty(RefPtr<ui::toolkit::PropertyEditor>(
                MakeRef<ui::toolkit::BoolEditor>(Allocator(), u8"Show travel", m_showTravel,
                                                 Function<void(bool)>{[self](bool v) { self->m_showTravel = v; }},
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
        // The events are a section list: the header's add icon places one at the playhead, and
        // each event is a section of its own with its remove icon (their order means nothing:
        // an event's time places it).
        auto events = MakeRef<app::ContainerListEditor>(Allocator(), StringView(u8"Events"),
                                                        StringView(u8"Events"));
        events->ElementsAsSections = true;
        for (const String& name : source.eventNames)
        {
            events->slotNames.PushBack(name);
        }
        events->OnAdd = [self]()
        {
            self->QueueStructural(u8"add-event",
                                  Function<void()>{[self]()
                                                   {
                                                       animation::AnimationClipSource& s =
                                                           self->m_asset->source;
                                                       s.eventTimes.PushBack(self->m_time);
                                                       s.eventNames.PushBack(String(u8"event"));
                                                   }});
        };
        g.AddProperty(RefPtr<ui::toolkit::PropertyEditor>(events.Get()));
        const usize eventCount = source.eventTimes.Size();
        for (usize e = 0; e < eventCount; ++e)
        {
            const String cat = EventSection(e);
            g.SetCategoryHeaderActions(
                cat.AsView(),
                app::ContainerListEditor::ElementActions(
                    Allocator(), e, eventCount, Function<void(usize, bool)>{},
                    [self](usize i)
                    {
                        self->QueueStructural(
                            u8"del-event",
                            Function<void()>{[self, i]()
                                             {
                                                 animation::AnimationClipSource& s =
                                                     self->m_asset->source;
                                                 if (i < s.eventTimes.Size())
                                                 {
                                                     s.eventTimes.RemoveAt(i);
                                                 }
                                                 if (i < s.eventNames.Size())
                                                 {
                                                     s.eventNames.RemoveAt(i);
                                                 }
                                             }});
                    }));
            g.AddProperty(RefPtr<ui::toolkit::PropertyEditor>(
                MakeRef<ui::toolkit::FloatEditor>(
                    Allocator(), u8"Time (s)", static_cast<f64>(source.eventTimes[e]), 0.0,
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
                    Allocator(), u8"Name", source.eventNames[e].AsView(),
                    Function<void(StringView)>{[self, &source, e](StringView v)
                                               {
                                                   source.eventNames[e] = String(v);
                                                   self->CommitEdit(u8"event-name");
                                               }},
                    cat.AsView())
                    .Get()));
        }
    }

    String AnimationClipEditorPage::EventSection(usize index)
    {
        return Format(u8"Event {}", index + 1);
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
                UniquePtr<IEditorCommand>(self->Allocator().New<EditClipCommand>(
                                              *self, key.AsView(), self->m_undoBaseline, after),
                                          self->Allocator()));
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
            Allocator().New<EditClipCommand>(*this, mergeKey, m_undoBaseline, after),
            Allocator()));
        m_undoBaseline = Move(after);
        MarkDirty();
    }

    // ============================ Frame / save / close ======================================

    ui::UIContext* AnimationClipEditorPage::Ctx() const
    {
        return (m_grid.Get() != nullptr) ? m_grid->Context : nullptr;
    }

    bool AnimationClipEditorPage::CanPlay() const
    {
        return m_clip.Get() != nullptr;
    }

    void AnimationClipEditorPage::Play()
    {
        // Resumes; a clip that ran to its end starts over.
        animation::AnimationClip* clip = m_clip.Get();
        if (clip != nullptr && m_time >= clip->duration)
        {
            m_time = 0.0f;
        }
        m_playing = true;
    }

    void AnimationClipEditorPage::Stop()
    {
        m_playing = false;
        m_time = 0.0f;
        SyncTimeSlider();
    }

    void AnimationClipEditorPage::Restart()
    {
        m_time = 0.0f;
        m_playing = true;
    }

    void AnimationClipEditorPage::SyncTimeSlider()
    {
        animation::AnimationClip* clip = m_clip.Get();
        if (m_timeSlider.Get() == nullptr || clip == nullptr || clip->duration <= 0.0f)
        {
            return;
        }
        m_scrubbing = true;
        m_timeSlider->Value.SetValue(m_time / clip->duration);
        m_scrubbing = false;
    }

    void AnimationClipEditorPage::OnUpdate(runtime::IApplicationHost&, f32 dt)
    {
        if (m_toolbar.Get() != nullptr)
        {
            m_toolbar->Refresh(); // the page's actions and the transport follow it each frame
        }
        UpdatePreview(dt);
        if (m_preview)
        {
            m_preview->Update(dt);
        }
    }

    void AnimationClipEditorPage::OnRenderWindow(runtime::IApplicationHost&,
                                                 foundation::graphics::FrameContext& frame)
    {
        if (m_preview)
        {
            m_preview->RenderFrame(frame);
        }
    }

    Status AnimationClipEditorPage::Save()
    {
        if (m_asset.Get() == nullptr || m_context->Project() == nullptr)
        {
            return Status{ErrorCode::NotFound};
        }
        foundation::content::Instance* instance =
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
            LOG_INFO(u8"Editor", u8"saved animation clip '{}'", m_title);
        }
        return saved;
    }

    void AnimationClipEditorPage::OnClose()
    {
        if (m_preview)
        {
            m_preview->Shutdown();
        }
    }

    // ============================ Factory ===================================================

    const TypeInfo* AnimationClipPageFactory::PrimaryType() const
    {
        return &pipeline::AnimationClipAsset::StaticType();
    }

    UniquePtr<EditorPage>
    AnimationClipPageFactory::CreatePage(EditorContext& context,
                                         foundation::content::Instance& instance)
    {
        auto* page =
            editor::EditorRootAllocator().New<AnimationClipEditorPage>(context, *m_host, *m_uiHost, instance);
        return UniquePtr<EditorPage>(page, editor::EditorRootAllocator());
    }

    void RegisterAnimationClipEditor(EditorContext& context, runtime::IApplicationHost& host,
                                     ui::runtime::UIHost& uiHost)
    {
        // The preview-prefs section (registered before the app loads the per-project store).
        GlobalTypeRegistry().Register(ClipPreviewSettings::StaticType(), TypeDomain(u8"Editor"));
        RegisterSerializable<ClipPreviewSettings>();

        context.Pages().Register(UniquePtr<IEditorPageFactory>(
            editor::EditorRootAllocator().New<AnimationClipPageFactory>(host, uiHost), editor::EditorRootAllocator()));
    }

    RTTI_DEFINE_OBJECT_VERSIONED(ClipPreviewSettings, "rtti::editor::editor.clip", 1)
}
