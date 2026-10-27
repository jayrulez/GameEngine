// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::PropertyAnimation - the `:panel` partition (implementation; see PropertyAnimationPanel.cppm).

module;
#include "Core/Prelude.h"

module editor.propertyanimation;

import foundation.core;
import foundation.scene;
import foundation.propertyanimation;
import foundation.gui;
import foundation.gui.toolkit;
import editor.core;
import editor.app; // FindById

using namespace foundation::core;
namespace propanim = foundation::propertyanimation;
namespace gui = foundation::gui;
namespace scene = foundation::scene;

namespace editor
{
    namespace
    {
        // The value's parts as a track's channels take them (a Float3's three, a Color's four).
        void KeyValue(propanim::PropertyTrack& track, f32 time, const Variant& value)
        {
            const auto key = [time](f32 v) { return CurveKey{time, v, 0.0f, 0.0f, CurveKeyInterpolation::Cubic, CurveTangentMode::Auto}; };
            const auto set = [&](u32 channel, f32 v)
            {
                Curve& curve = track.channels[channel];
                for (CurveKey& existing : curve.Keys())
                    if (Abs(existing.time - time) < 1e-4f)
                    {
                        existing.value = v; // a key already there takes the value
                        curve.ApplyTangentModes();
                        return;
                    }
                curve.AddKey(key(v));
                curve.ApplyTangentModes();
            };
            switch (track.kind)
            {
            case propanim::TrackValueKind::Float:
                if (value.Is<f32>())
                    set(0, value.Get<f32>());
                break;
            case propanim::TrackValueKind::Float3:
                if (value.Is<Float3>())
                {
                    const Float3 v = value.Get<Float3>();
                    set(0, v.x);
                    set(1, v.y);
                    set(2, v.z);
                }
                break;
            case propanim::TrackValueKind::Color:
                if (value.Is<Color>())
                {
                    const Color c = value.Get<Color>();
                    set(0, c.r);
                    set(1, c.g);
                    set(2, c.b);
                    set(3, c.a);
                }
                break;
            case propanim::TrackValueKind::Quat:
                if (value.Is<Quaternion>())
                {
                    for (propanim::QuatKey& existing : track.quatKeys)
                        if (Abs(existing.time - time) < 1e-4f)
                        {
                            existing.value = value.Get<Quaternion>();
                            return;
                        }
                    usize at = 0;
                    while (at < track.quatKeys.Size() && track.quatKeys[at].time < time)
                        ++at;
                    track.quatKeys.Insert(at, propanim::QuatKey{time, value.Get<Quaternion>()});
                }
                break;
            }
        }

        [[nodiscard]] bool HasTrack(const propanim::PropertyAnimationClip& clip, StringView componentType, StringView propertyPath)
        {
            for (const propanim::PropertyTrack& track : clip.tracks)
                if (track.componentType.AsView() == componentType && track.propertyPath.AsView() == propertyPath)
                    return true;
            return false;
        }
    }

    template <typename T>
    T* PropertyAnimationPanel::Find(StringView id) const
    {
        return m_content ? app::FindById<T>(*m_content, id) : nullptr;
    }

    PropertyAnimationPanel::PropertyAnimationPanel(EditorContext& context, scene::Scene& scene, RefPtr<gui::Node> content)
        : m_context(&context), m_scene(&scene), m_content(Move(content)), m_document(context), m_source(m_document), m_preview(scene)
    {
        PropertyAnimationPanel* self = this;
        m_document.OnChanged = [self]() { self->Changed(); };
        m_source.CurrentValue = [self](const propanim::PropertyTrack& track) { return self->EntityValue(track.componentType.AsView(), track.propertyPath.AsView()); };

        // One axis under the ruler, the dopesheet and the curve view; a scrub poses the entity.
        m_axis = MakeRef<gui::TimeAxis>(EditorRootAllocator());
        m_axis->SetPixelsPerSecond(120.0f);
        m_axis->SetFrameRate(30.0f);
        m_axis->SetDuration(m_document.Length());
        m_axis->SetOnPlayheadChanged([self](f32 time)
                                     {
                                         if (self->m_moving)
                                             return;
                                         self->m_transport.SetTime(time);
                                         self->Pose();
                                     });
        if (auto* ruler = Find<gui::TimeRuler>(u8"anim-ruler"))
            ruler->SetTimeAxis(m_axis);
        m_sheet = Find<gui::Dopesheet>(u8"anim-sheet");
        if (m_sheet != nullptr)
        {
            m_sheet->SetTimeAxis(m_axis);
            m_sheet->SetSource(&m_source);
        }
        m_curves = Find<gui::CurveView>(u8"anim-curves");
        if (m_curves != nullptr)
        {
            m_curves->SetTimeAxis(m_axis);
            m_curves->SetSource(&m_source);
        }

        m_clipName = Find<gui::Label>(u8"anim-clip");
        m_dirty = Find<gui::Node>(u8"anim-dirty");
        m_entityName = Find<gui::Label>(u8"anim-entity");
        m_empty = Find<gui::Node>(u8"anim-empty");
        m_body = Find<gui::Node>(u8"anim-body");
        m_length = Find<gui::NumberField>(u8"anim-length");
        if (m_length != nullptr)
            m_length->SetOnValueChanged([self](f64 value) { self->SetLength(static_cast<f32>(value)); });
        const auto button = [this](StringView id, Function<void()> click) -> gui::ButtonBase*
        {
            auto* found = Find<gui::ButtonBase>(id);
            if (found != nullptr)
                found->SetOnClick(Move(click));
            return found;
        };
        (void)button(u8"anim-new", [self]()
                     {
                         if (self->RequestNewClip)
                             self->RequestNewClip();
                     });
        (void)button(u8"anim-open", [self]()
                     {
                         if (self->RequestOpenClip)
                             self->RequestOpenClip();
                     });
        m_save = button(u8"anim-save", [self]() { (void)self->Save(); });
        m_undo = button(u8"anim-undo", [self]() { self->Undo(); });
        m_redo = button(u8"anim-redo", [self]() { self->Redo(); });
        (void)button(u8"anim-bind", [self]() { self->BindSelected(); }); // always on: the page's selection is read when clicked
        m_addTrack = button(u8"anim-add-track", [self]() { self->OpenTrackMenu(); });
        m_key = button(u8"anim-key", [self]() { (void)self->KeyChosen(); });
        m_remove = button(u8"anim-remove", [self]() { (void)self->RemoveChosen(); });
        m_restart = button(u8"anim-restart", [self]() { self->Restart(); });
        (void)button(u8"anim-frame", [self]() { self->FrameCurves(); });
        (void)button(u8"anim-close", [self]()
                     {
                         if (self->RequestClose)
                             self->RequestClose();
                     });
        m_stop = button(u8"anim-stop", [self]() { self->Stop(); });
        m_play = Find<gui::ButtonBase>(u8"anim-play");
        if (m_play != nullptr)
            m_play->SetOnToggled([self](bool on)
                                 {
                                     if (on)
                                         self->Play();
                                     else
                                         self->Pause();
                                 });
        if (auto* loop = Find<gui::ButtonBase>(u8"anim-loop"))
            loop->SetOnToggled([self](bool on) { self->SetLooping(on); });

        // The keys it binds, while it holds the focus: the clip's, never the scene's.
        if (m_content)
        {
            m_commands.PushBack(m_content->OnCommand(u8"edit.undo", [self](gui::CommandEvent&) { self->Undo(); }));
            m_commands.PushBack(m_content->OnCommand(u8"edit.redo", [self](gui::CommandEvent&) { self->Redo(); }));
            m_commands.PushBack(m_content->OnCommand(u8"file.save", [self](gui::CommandEvent&) { (void)self->Save(); }));
        }
        Changed();
    }

    PropertyAnimationPanel::~PropertyAnimationPanel()
    {
        m_preview.Stop();
        if (m_sheet != nullptr)
            m_sheet->SetSource(nullptr);
        if (m_curves != nullptr)
            m_curves->SetSource(nullptr);
        if (m_content)
            for (const u32 token : m_commands)
                (void)m_content->RemoveCommandHandler(token);
    }

    // === The clip ===

    bool PropertyAnimationPanel::LoadClip(const Guid& clipId)
    {
        m_preview.Stop();
        if (!m_document.Load(clipId))
            return false;
        m_transport.Stop();
        m_moving = true;
        m_axis->SetPlayhead(0.0f);
        m_moving = false;
        if (m_sheet != nullptr)
            m_sheet->SetSelection(Span<const gui::TimelineKeyRef>{});
        return true;
    }

    void PropertyAnimationPanel::RequestEditClip(const Guid& clipId, const Guid& entity)
    {
        PropertyAnimationPanel* self = this;
        Function<void()> open = [self, clipId, entity]()
        {
            if (clipId != self->m_document.ClipId())
                (void)self->LoadClip(clipId);
            if (!entity.IsNil())
                self->BindEntity(entity);
        };
        if (GuardUnsaved && m_document.IsDirty() && clipId != m_document.ClipId())
            GuardUnsaved(Move(open));
        else
            open();
    }

    Status PropertyAnimationPanel::Save()
    {
        if (!m_document.HasClip())
            return Status{ErrorCode::NotFound};
        const Status saved = m_document.Save();
        if (saved.IsOk())
            m_context->Notify(NoticeKind::Info, Format(u8"Saved the clip '{}'.", m_document.Name()).AsView());
        else
            m_context->Notify(NoticeKind::Warning, Format(u8"The clip '{}' did not save; the Console says why.", m_document.Name()).AsView());
        return saved;
    }

    void PropertyAnimationPanel::Undo()
    {
        if (m_document.Commands().CanUndo())
            m_document.Commands().Undo();
    }

    void PropertyAnimationPanel::Redo()
    {
        if (m_document.Commands().CanRedo())
            m_document.Commands().Redo();
    }

    void PropertyAnimationPanel::SetLength(f32 seconds)
    {
        m_document.Edit(u8"propanim-length", [seconds](propanim::PropertyAnimationClip& clip) { clip.duration = Max(0.0f, seconds); });
    }

    // === The entity ===

    void PropertyAnimationPanel::BindEntity(const Guid& entity)
    {
        if (entity == m_entity)
            return;
        m_preview.Stop(); // the one posed goes back first
        m_entity = entity;
        Pose();
        ShowBar();
    }

    void PropertyAnimationPanel::BindSelected()
    {
        if (SelectedEntity)
            BindEntity(SelectedEntity());
    }

    Variant PropertyAnimationPanel::EntityValue(StringView componentType, StringView propertyPath) const
    {
        return m_preview.Read(m_entity, componentType, propertyPath);
    }

    // === The tracks ===

    Array<AnimatablePropertyInfo> PropertyAnimationPanel::OfferedTracks() const
    {
        Array<AnimatablePropertyInfo> offered;
        for (AnimatablePropertyInfo& seed : m_preview.TrackSeeds(m_entity))
            if (!HasTrack(m_document.Clip(), seed.componentType.AsView(), seed.propertyPath.AsView()))
                offered.PushBack(Move(seed));
        return offered;
    }

    void PropertyAnimationPanel::AddTrack(const AnimatablePropertyInfo& seed)
    {
        if (!m_document.HasClip() || HasTrack(m_document.Clip(), seed.componentType.AsView(), seed.propertyPath.AsView()))
            return;
        const Variant value = EntityValue(seed.componentType.AsView(), seed.propertyPath.AsView());
        const f32 time = m_transport.Time();
        m_preview.Stop(); // the tracks change: what the preview keeps is taken again
        m_document.Edit(u8"propanim-add-track", [&seed, &value, time](propanim::PropertyAnimationClip& clip)
                        {
                            propanim::PropertyTrack track;
                            track.componentType = seed.componentType;
                            track.propertyPath = seed.propertyPath;
                            track.kind = seed.kind;
                            if (!value.IsEmpty())
                                KeyValue(track, time, value); // it starts holding what the entity shows
                            clip.tracks.PushBack(Move(track));
                        });
    }

    Array<usize> PropertyAnimationPanel::ChosenTracks() const
    {
        Array<usize> tracks;
        if (m_sheet == nullptr)
            return tracks;
        const Span<const ClipSource::Channel> channels = m_source.Channels();
        for (const gui::TimelineKeyRef& key : m_sheet->GetSelection())
        {
            if (key.Channel >= channels.Size())
                continue;
            const usize track = channels[key.Channel].Track;
            bool have = false;
            for (const usize known : tracks)
                have = have || known == track;
            if (!have)
                tracks.PushBack(track);
        }
        return tracks;
    }

    usize PropertyAnimationPanel::KeyChosen()
    {
        if (!m_document.HasClip() || m_entity.IsNil())
            return 0;
        Array<usize> tracks = ChosenTracks();
        if (tracks.IsEmpty())
            for (usize t = 0; t < m_document.Clip().tracks.Size(); ++t)
                tracks.PushBack(t);
        // What the entity shows, read before the edit (the preview poses it after).
        Array<Variant> values;
        for (const usize t : tracks)
        {
            const propanim::PropertyTrack& track = m_document.Clip().tracks[t];
            values.PushBack(EntityValue(track.componentType.AsView(), track.propertyPath.AsView()));
        }
        usize keyed = 0;
        for (const Variant& value : values)
            keyed += value.IsEmpty() ? 0u : 1u;
        if (keyed == 0)
            return 0;
        const f32 time = m_transport.Time();
        m_document.Edit(u8"propanim-key", [&tracks, &values, time](propanim::PropertyAnimationClip& clip)
                        {
                            for (usize i = 0; i < tracks.Size(); ++i)
                                if (!values[i].IsEmpty())
                                    KeyValue(clip.tracks[tracks[i]], time, values[i]);
                        });
        return keyed;
    }

    bool PropertyAnimationPanel::RemoveChosen()
    {
        Array<usize> tracks = ChosenTracks();
        if (tracks.IsEmpty())
            return false;
        m_preview.Stop(); // what it wrote goes back before its tracks go
        if (m_sheet != nullptr)
            m_sheet->SetSelection(Span<const gui::TimelineKeyRef>{});
        m_document.Edit(u8"propanim-remove-tracks", [&tracks](propanim::PropertyAnimationClip& clip)
                        {
                            // Last first, so each index still names its track.
                            for (usize i = 1; i < tracks.Size(); ++i)
                            {
                                const usize track = tracks[i];
                                usize j = i;
                                while (j > 0 && tracks[j - 1] < track)
                                {
                                    tracks[j] = tracks[j - 1];
                                    --j;
                                }
                                tracks[j] = track;
                            }
                            for (const usize track : tracks)
                                clip.tracks.RemoveAt(track);
                        });
        return true;
    }

    void PropertyAnimationPanel::OpenTrackMenu()
    {
        if (m_addTrack == nullptr)
            return;
        // Made fresh each time: what it offers follows the entity and the tracks.
        m_trackMenu = MakeRef<gui::Menu>(EditorRootAllocator());
        m_trackMenu->SetAutoWidth(true);
        PropertyAnimationPanel* self = this;
        const Array<AnimatablePropertyInfo> offered = OfferedTracks();
        for (const AnimatablePropertyInfo& seed : offered)
        {
            const AnimatablePropertyInfo pick = seed;
            (void)m_trackMenu->AddItem(Format(u8"{}.{}", seed.componentType, seed.propertyPath).AsView(), [self, pick]() { self->AddTrack(pick); });
        }
        if (offered.IsEmpty())
            m_trackMenu->AddItem(m_entity.IsNil() ? StringView(u8"Bind an entity first") : StringView(u8"Every property has a track"), []() {})->SetEnabled(false);
        m_trackMenu->OpenBelow(*m_addTrack, *m_addTrack);
    }

    // === The transport ===

    void PropertyAnimationPanel::Play()
    {
        m_transport.Play(m_document.Length());
        ShowBar();
    }

    void PropertyAnimationPanel::Pause()
    {
        m_transport.Pause();
        ShowBar();
    }

    void PropertyAnimationPanel::Stop()
    {
        m_transport.Stop();
        m_moving = true;
        m_axis->SetPlayhead(0.0f);
        m_moving = false;
        Pose(); // the start pose
        ShowBar();
    }

    void PropertyAnimationPanel::Restart()
    {
        m_transport.Stop();
        m_transport.Play(m_document.Length());
        m_moving = true;
        m_axis->SetPlayhead(0.0f);
        m_moving = false;
        Pose();
        ShowBar();
    }

    void PropertyAnimationPanel::SetLooping(bool looping)
    {
        m_transport.SetLooping(looping);
    }

    void PropertyAnimationPanel::Scrub(f32 time)
    {
        m_axis->SetPlayhead(time); // as the ruler's scrub: the transport and the pose follow
    }

    void PropertyAnimationPanel::FrameCurves()
    {
        if (m_curves != nullptr)
            m_curves->FrameAll();
    }

    void PropertyAnimationPanel::FitValues()
    {
        if (m_curves == nullptr)
            return;
        f32 low = 0.0f;
        f32 high = 0.0f;
        bool any = false;
        for (usize c = 0; c < m_source.CurveCount(); ++c)
            for (const CurveKey& key : m_source.GetKeys(c))
            {
                low = any ? Min(low, key.value) : key.value;
                high = any ? Max(high, key.value) : key.value;
                any = true;
            }
        if (!any)
        {
            low = 0.0f;
            high = 1.0f;
        }
        if (high - low < 1e-3f)
        {
            low -= 1.0f; // one value: a span about it
            high += 1.0f;
        }
        const f32 margin = (high - low) * 0.1f;
        m_curves->SetValueRange(low - margin, high + margin);
    }

    void PropertyAnimationPanel::Update(f32 dt, bool simulating)
    {
        if (simulating != m_simulating)
        {
            m_simulating = simulating;
            if (simulating)
            {
                m_transport.Pause(); // the scene's own animators run; the preview stands down
                m_preview.Stop();
            }
            ShowBar();
        }
        if (!m_simulating && m_transport.Advance(dt, m_document.Length()))
        {
            m_moving = true;
            m_axis->SetPlayhead(m_transport.Time());
            m_moving = false;
            Pose();
            if (!m_transport.IsPlaying())
                ShowBar(); // it stopped at the end
        }
        // The dopesheet tells no one of its choice: the curve view follows it here.
        ShowCurves();
        if (m_fitValues)
        {
            m_fitValues = false;
            FitValues();
        }
    }

    // === Showing it ===

    void PropertyAnimationPanel::Changed()
    {
        m_source.Refresh();
        m_axis->SetDuration(m_document.Length());
        // Another clip, or tracks come or gone: the curves shown are others (a key's edit is not).
        if (m_document.ClipId() != m_shownClip || m_document.Clip().tracks.Size() != m_trackCount)
        {
            m_shownClip = m_document.ClipId();
            m_trackCount = m_document.Clip().tracks.Size();
            m_fitValues = true;
        }
        // The length it plays over (a new clip's second, its last key's time when later).
        if (m_length != nullptr && static_cast<f32>(m_length->GetValue()) != m_document.Length())
            m_length->SetValue(static_cast<f64>(m_document.Length()));
        Pose(); // an edit, an undo: the entity shows the clip as it is now
        ShowBar();
    }

    void PropertyAnimationPanel::Pose()
    {
        if (m_simulating || !m_document.HasClip() || m_entity.IsNil() || m_document.Clip().tracks.IsEmpty())
        {
            m_preview.Stop();
            return;
        }
        m_preview.Pose(m_entity, m_document.Clip(), m_transport.Time());
    }

    void PropertyAnimationPanel::ShowCurves()
    {
        Array<usize> tracks = ChosenTracks();
        if (tracks.IsEmpty())
            for (usize t = 0; t < m_document.Clip().tracks.Size(); ++t)
                tracks.PushBack(t); // none chosen: every track's
        bool same = tracks.Size() == m_shownTracks.Size();
        for (usize i = 0; same && i < tracks.Size(); ++i)
            same = tracks[i] == m_shownTracks[i];
        if (same && tracks.Size() == m_source.ShownTracks().Size())
            return;
        m_shownTracks = tracks;
        m_source.ShowTracks(tracks.AsSpan());
        m_fitValues = true; // other curves: their values fill the view
    }

    void PropertyAnimationPanel::ShowBar()
    {
        const bool clip = m_document.HasClip();
        if (m_clipName != nullptr)
            m_clipName->SetText(clip ? m_document.Name() : StringView(u8"No clip"));
        if (m_dirty != nullptr)
            m_dirty->SetDisplayed(m_document.IsDirty());
        if (m_empty != nullptr)
            m_empty->SetDisplayed(!clip);
        if (m_body != nullptr)
            m_body->SetDisplayed(clip);
        if (m_entityName != nullptr)
        {
            const scene::EntityHandle handle = m_entity.IsNil() ? scene::EntityHandle{} : m_scene->FindEntity(m_entity);
            m_entityName->SetText(handle.IsAssigned() ? m_scene->GetEntityName(handle) : StringView(u8"No entity"));
        }
        const bool editable = clip && !m_simulating;
        const auto enable = [](gui::ButtonBase* button, bool on)
        {
            if (button != nullptr)
                button->SetEnabled(on);
        };
        enable(m_save, m_document.IsDirty());
        enable(m_undo, m_document.Commands().CanUndo());
        enable(m_redo, m_document.Commands().CanRedo());
        enable(m_addTrack, editable && !m_entity.IsNil());
        enable(m_key, editable && !m_entity.IsNil() && !m_document.Clip().tracks.IsEmpty());
        enable(m_remove, editable);
        enable(m_play, editable);
        enable(m_restart, editable);
        enable(m_stop, editable);
        if (m_play != nullptr)
            m_play->SetChecked(m_transport.IsPlaying());
    }
}
