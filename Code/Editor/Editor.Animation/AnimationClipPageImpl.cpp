// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Animation - the `:clip_page` partition (implementation).

module;
#include <cmath> // std::fmod, to wrap a looping clip's clock
#include "Core/Prelude.h"
#include "Core/Log/Log.h"
#include "Core/Reflection/Reflect.h"

module editor.animation;

import foundation.core;
import foundation.settings; // per-project editor settings, which keep the preview's rig per clip
import foundation.content;
import foundation.graphics;
import foundation.runtime;
import foundation.resource;
import foundation.animation;
import foundation.animation.resource;
import animation.pipeline;
import foundation.gui;
import foundation.gui.toolkit;
import editor.core;
import editor.app;
import editor.preview;

using namespace foundation::core;

namespace editor
{
    namespace gui = foundation::gui;

    // The preview's rig per clip ({clip -> skeleton, skinned mesh}), a section of the per-project
    // editor settings, so a clip opened again plays on the rig it used last. Its type id must not
    // change, or settings already saved under it stop reading.
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
        void Serialize(ISerializer& ar) override { foundation::core::Serialize(ar, "prefs", prefs); }
    };

    namespace
    {
        constexpr StringView kEvents = u8"events";
        constexpr StringView kNewEventName = u8"Event";

        // The index of the event a row's path names ("events.2.time" gives 2), or -1.
        [[nodiscard]] i32 EventOf(StringView path)
        {
            StringView element = path;
            if (gui::ParentPropertyPath(element) != kEvents)
                element = gui::ParentPropertyPath(path);
            if (gui::ParentPropertyPath(element) != kEvents)
                return -1;
            const StringView index = element.SubStr(kEvents.Size() + 1, element.Size() - kEvents.Size() - 1);
            const Optional<i64> parsed = ParseInt(index);
            return parsed.HasValue() && parsed.Value() >= 0 ? static_cast<i32>(parsed.Value()) : -1;
        }

        [[nodiscard]] StringView LastSegment(StringView path)
        {
            for (usize i = path.Size(); i-- > 0;)
                if (path[i] == u8'.')
                    return path.SubStr(i + 1, path.Size() - i - 1);
            return path;
        }

        // Makes the events' two arrays the same length, whatever an older clip saved.
        void Square(foundation::animation::AnimationClipSource& source)
        {
            while (source.eventNames.Size() < source.eventTimes.Size())
                source.eventNames.PushBack(String{});
            while (source.eventTimes.Size() < source.eventNames.Size())
                source.eventTimes.PushBack(0.0f);
        }

        // Always two decimals, so the text keeps its width while the time runs ("0.90 s", "0.95 s").
        [[nodiscard]] String Seconds(f32 seconds)
        {
            const i64 cents = static_cast<i64>(Max(seconds, 0.0f) * 100.0f + 0.5f);
            return Format(u8"{}.{}{} s", cents / 100, (cents % 100) / 10, cents % 10);
        }
    }

    AnimationClipEditorPage::AnimationClipEditorPage(EditorContext& context, app::AssetPageServices services, foundation::content::Instance& instance)
        : app::AssetObjectPage(context, Move(services), instance, pipeline::AnimationClipAsset::StaticType(), kPath)
    {
        // The tracks and keys come from the import, so the page builds its own rows.
        HideAssetRows();
        Provide<IPlaybackPage>(*this);
        AnimationClipEditorPage* self = this;
        BuildRows();

        m_axis = MakeRef<gui::TimeAxis>(Allocator());
        m_axis->SetFrameRate(30.0f);
        m_axis->SetDuration(Length());
        m_axis->SetOnPlayheadChanged([self](f32 time)
                                     {
                                         if (self->m_moving)
                                             return;
                                         // A scrub: the preview stays at the time it is moved to.
                                         self->m_time = time;
                                         self->m_playing = false;
                                     });
        if (Content())
        {
            m_pane = MakeUnique<app::PreviewPane>(Allocator(), *Content(), Services(), u8"animclip.preview");
            m_pane->OnFrame = [self]() { self->FramePreview(); };
            m_rig = MakeUnique<AnimationPreviewRig>(Allocator(), Context(), Services(), *m_pane);
            m_rig->OnPicked = [self]() { self->SavePreviewPref(); };
            m_rig->OnFirstShown = [self]() { self->FramePreview(); };
            BindAction(Find<gui::ButtonBase>(u8"clip-restart"), u8"playback.restart");
            BindAction(Find<gui::ButtonBase>(u8"clip-play"), u8"playback.play");
            BindAction(Find<gui::ButtonBase>(u8"clip-stop"), u8"playback.stop");
            m_state = Find<gui::Label>(u8"clip-state");
            m_travelButton = Find<gui::ButtonBase>(u8"clip-travel");
            if (m_travelButton != nullptr)
                m_travelButton->SetOnClick([self]() { self->SetTravel(!self->m_travel); });
            m_ruler = Find<gui::TimeRuler>(u8"clip-ruler");
            if (m_ruler != nullptr)
            {
                m_ruler->SetTimeAxis(m_axis);
                m_ruler->SetMarkerSource(this);
                m_ruler->OnMarkerChosen = [self](i32 marker) { self->ChooseEvent(marker); };
            }
        }
        if (Context().Resources() != nullptr)
            m_clip = Context().Resources()->Bind<foundation::animation::AnimationClip>(InstanceId());
        LoadPreviewPref();
        ShowRows();
        ShowFacts();
    }

    AnimationClipEditorPage::~AnimationClipEditorPage()
    {
        if (m_ruler != nullptr)
        {
            m_ruler->SetMarkerSource(nullptr);
            m_ruler->OnMarkerChosen = nullptr;
        }
    }

    pipeline::AnimationClipAsset* AnimationClipEditorPage::Clip() const
    {
        return ObjectAs<pipeline::AnimationClipAsset>();
    }

    f32 AnimationClipEditorPage::Length() const
    {
        const foundation::animation::AnimationClip* cooked = m_clip ? m_clip.Get() : nullptr;
        if (cooked != nullptr && cooked->duration > 0.0f)
            return cooked->duration;
        const pipeline::AnimationClipAsset* clip = Clip();
        return clip != nullptr && clip->source.duration > 0.0f ? clip->source.duration : 1.0f;
    }

    // === The rows ===

    void AnimationClipEditorPage::BuildRows()
    {
        m_clipRows = MakeUnique<gui::PropertyBag>(Allocator(), Allocator());
        m_rootRows = MakeUnique<gui::PropertyBag>(Allocator(), Allocator());
        m_eventRows = MakeUnique<gui::PropertyBag>(Allocator(), Allocator());

        gui::PropertyInfo& looping = m_clipRows->Add(u8"looping", gui::PropertyKind::Bool, gui::Variant(false));
        looping.DisplayName = String(u8"Looping");
        looping.Description = String(u8"Whether it plays again from its start when it ends (an animator may say otherwise)");

        gui::PropertyInfo& bone = m_rootRows->Add(u8"bone", gui::PropertyKind::String, gui::Variant(StringView{}));
        bone.DisplayName = String(u8"Root bone");
        bone.Description = String(u8"The bone its travel is read from, by name (empty: the skeleton's first root)");
        const auto axis = [&](StringView path, StringView shown, StringView about)
        {
            gui::PropertyInfo& info = m_rootRows->Add(path, gui::PropertyKind::Bool, gui::Variant(false));
            info.DisplayName = String(shown);
            info.Description = String(about);
        };
        axis(u8"horizontal", u8"Travel", u8"Its travel along the ground taken out of the pose, for the character to move by");
        axis(u8"vertical", u8"Height", u8"Its rise and fall taken out too (a jump that should lift the character)");
        axis(u8"yaw", u8"Turning", u8"Its turning about the vertical taken out, for the character to turn by");

        // The events are a list like the other pages' lists: each is a group of its time and its
        // name, and a new one goes at the playhead. Their order carries no meaning (each event's time
        // places it), so the list does not offer moving them.
        gui::PropertyInfo& list = m_eventRows->AddList(kEvents, gui::PropertyKind::Group, gui::Variant{});
        list.DisplayName = String(u8"Events");
        list.Description = String(u8"What the clip tells its listeners as it plays past each (a footstep, a hit); each a marker on the ruler");
        list.ListOps = gui::PropertyListAdd | gui::PropertyListRemove;
        if (gui::PropertyInfo* time = m_eventRows->AddElementField(kEvents, u8"time", gui::PropertyKind::Float, gui::Variant(0.0)))
        {
            time->DisplayName = String(u8"Time");
            time->Description = String(u8"When it fires, in seconds from the clip's start (kept within the clip)");
            time->Step = 0.01;
        }
        if (gui::PropertyInfo* name = m_eventRows->AddElementField(kEvents, u8"name", gui::PropertyKind::String, gui::Variant(kNewEventName)))
        {
            name->DisplayName = String(u8"Name");
            name->Description = String(u8"What its listeners are told");
        }

        AnimationClipEditorPage* self = this;
        gui::PropertyBag* const bags[] = {m_clipRows.Get(), m_rootRows.Get(), m_eventRows.Get()};
        for (gui::PropertyBag* raw : bags)
        {
            gui::PropertyBag* rows = raw;
            rows->SetOnCommit([self, rows](const gui::PropertyBag::Commit& commit)
                              {
                                  if (!commit.After.IsEmpty())
                                      self->Committed(*rows, commit.Path.AsView(), commit.After[0]);
                              });
        }
        m_eventRows->SetOnListEdited([self](StringView, const gui::ListEdit& edit) { self->EditEvents(edit); });
        // Each event is titled by its name and, while closed, shows when it fires.
        m_eventRows->SetOnDescribe([self](StringView path, gui::PropertyInfo& info)
                                   {
                                       if (gui::ParentPropertyPath(path) != kEvents)
                                           return;
                                       const i32 index = EventOf(path);
                                       const pipeline::AnimationClipAsset* clip = self->Clip();
                                       if (clip == nullptr || index < 0 || static_cast<usize>(index) >= clip->source.eventTimes.Size())
                                           return;
                                       const usize at = static_cast<usize>(index);
                                       const StringView name = at < clip->source.eventNames.Size() ? clip->source.eventNames[at].AsView() : StringView{};
                                       info.DisplayName = name.IsEmpty() ? Format(u8"Event {}", index + 1) : String(name);
                                       info.Summary = Format(u8"at {}", Seconds(clip->source.eventTimes[at]));
                                   });
        AddSection(u8"clip", u8"Clip", *m_clipRows);
        AddSection(u8"root", u8"Root motion", *m_rootRows);
        AddSection(u8"events", u8"Events", *m_eventRows);
    }

    void AnimationClipEditorPage::ShowRows()
    {
        const pipeline::AnimationClipAsset* clip = Clip();
        if (clip == nullptr)
            return;
        const foundation::animation::AnimationClipSource& source = clip->source;
        m_clipRows->SetValue(u8"looping", 0, gui::Variant(source.isLooping));
        m_rootRows->SetValue(u8"bone", 0, gui::Variant(source.rootMotion.rootBone.AsView()));
        m_rootRows->SetValue(u8"horizontal", 0, gui::Variant(source.rootMotion.horizontal));
        m_rootRows->SetValue(u8"vertical", 0, gui::Variant(source.rootMotion.vertical));
        m_rootRows->SetValue(u8"yaw", 0, gui::Variant(source.rootMotion.yaw));
        const usize count = Max(source.eventTimes.Size(), source.eventNames.Size()); // an edit makes both the same length
        m_eventRows->ResizeList(kEvents, 0, count);
        for (usize i = 0; i < count; ++i)
        {
            const String element = Format(u8"{}.{}", kEvents, i);
            const f32 time = i < source.eventTimes.Size() ? source.eventTimes[i] : 0.0f;
            const StringView name = i < source.eventNames.Size() ? source.eventNames[i].AsView() : StringView{};
            m_eventRows->SetValue(gui::JoinPropertyPath(element.AsView(), u8"time").AsView(), 0, gui::Variant(static_cast<f64>(time)));
            m_eventRows->SetValue(gui::JoinPropertyPath(element.AsView(), u8"name").AsView(), 0, gui::Variant(name));
        }
        m_eventRows->PropertiesChanged(); // describe the names and summaries again
    }

    void AnimationClipEditorPage::ShowFacts()
    {
        const pipeline::AnimationClipAsset* clip = Clip();
        if (clip == nullptr)
            return;
        const foundation::animation::AnimationClipSource& source = clip->source;
        SetFact(u8"Name", source.name.IsEmpty() ? StringView(u8"Unnamed") : source.name.AsView());
        SetFact(u8"Duration", Seconds(source.duration).AsView());
        SetFact(u8"Tracks", Format(u8"{}", source.trackBone.Size()).AsView());
        SetFact(u8"Keys", Format(u8"{}", source.keyTimes.Size()).AsView());
    }

    void AnimationClipEditorPage::Committed(gui::PropertyBag& rows, StringView path, const gui::Variant& value)
    {
        if (Clip() == nullptr)
            return;
        using Source = foundation::animation::AnimationClipSource;
        const auto edit = [this](const Function<void(Source&)>& mutate)
        { EditWhole(u8"edit_animclip", [&mutate](ISerializable& object) { mutate(static_cast<pipeline::AnimationClipAsset&>(object).source); }); };
        if (&rows == m_clipRows.Get())
        {
            const bool looping = value.AsBool();
            edit([looping](Source& source) { source.isLooping = looping; });
            return;
        }
        if (&rows == m_rootRows.Get())
        {
            if (path == u8"bone")
            {
                const String bone(Trim(value.AsString()));
                edit([&bone](Source& source) { source.rootMotion.rootBone = bone; });
                return;
            }
            const bool on = value.AsBool();
            bool foundation::animation::RootMotionSettings::*field = path == u8"horizontal" ? &foundation::animation::RootMotionSettings::horizontal
                                                                     : path == u8"vertical" ? &foundation::animation::RootMotionSettings::vertical
                                                                                            : &foundation::animation::RootMotionSettings::yaw;
            edit([field, on](Source& source) { source.rootMotion.*field = on; });
            return;
        }
        const i32 index = EventOf(path);
        if (index < 0 || static_cast<usize>(index) >= Clip()->source.eventTimes.Size())
            return;
        const usize at = static_cast<usize>(index);
        const StringView field = LastSegment(path);
        if (field == u8"time")
        {
            const f32 time = static_cast<f32>(Clamp(value.AsFloat(), 0.0, static_cast<f64>(Length())));
            edit([at, time](Source& source)
                 {
                     Square(source);
                     source.eventTimes[at] = time;
                 });
        }
        else if (field == u8"name")
        {
            const String name(value.AsString());
            edit([at, &name](Source& source)
                 {
                     Square(source);
                     source.eventNames[at] = name;
                 });
        }
    }

    void AnimationClipEditorPage::EditEvents(const gui::ListEdit& edit)
    {
        const pipeline::AnimationClipAsset* clip = Clip();
        if (clip == nullptr)
            return;
        if (edit.Kind == gui::ListEdit::Op::Insert)
        {
            // Added at the playhead, at the end of the list: its place in the list means nothing.
            const f32 time = Clamp(m_time, 0.0f, Length());
            EditWhole(u8"edit_animclip", [time](ISerializable& object)
                      {
                          auto& source = static_cast<pipeline::AnimationClipAsset&>(object).source;
                          Square(source);
                          source.eventTimes.PushBack(time);
                          source.eventNames.PushBack(String(kNewEventName));
                      });
        }
        else if (edit.Kind == gui::ListEdit::Op::Remove && edit.Index < clip->source.eventTimes.Size())
        {
            const usize index = edit.Index;
            EditWhole(u8"edit_animclip", [index](ISerializable& object)
                      {
                          auto& source = static_cast<pipeline::AnimationClipAsset&>(object).source;
                          Square(source);
                          source.eventTimes.RemoveAt(index);
                          source.eventNames.RemoveAt(index);
                      });
        }
        ShowRows(); // show the list as the clip holds it, undoing any move (the list does not offer moves)
    }

    void AnimationClipEditorPage::OnObjectChanged()
    {
        ShowRows();
        ShowFacts();
        NotifyTimeMarkersChanged();
    }

    void AnimationClipEditorPage::ChooseEvent(i32 index)
    {
        const pipeline::AnimationClipAsset* clip = Clip();
        if (clip == nullptr || index < 0 || static_cast<usize>(index) >= clip->source.eventTimes.Size())
            return;
        if (m_ruler != nullptr && m_ruler->GetChosenMarker() != index)
            m_ruler->ChooseMarker(index); // the ruler calls back into this, and the call below opens the rows
        gui::PropertyGrid* grid = Grid();
        if (grid == nullptr)
            return;
        grid->SetOpen(Sections().PathIn(u8"events", kEvents).AsView(), true);
        const String path = Sections().PathIn(u8"events", Format(u8"{}.{}", kEvents, index).AsView());
        grid->SetOpen(path.AsView(), true);
        const i32 found = grid->FindRow(path.AsView());
        if (found >= 0)
            grid->ScrollToItem(found, gui::ScrollAlign::Start);
    }

    // === The events as the ruler's markers ===

    usize AnimationClipEditorPage::MarkerCount() const
    {
        const pipeline::AnimationClipAsset* clip = Clip();
        return clip != nullptr ? clip->source.eventTimes.Size() : 0;
    }

    gui::TimeMarker AnimationClipEditorPage::GetMarker(usize marker) const
    {
        const pipeline::AnimationClipAsset* clip = Clip();
        if (clip == nullptr || marker >= clip->source.eventTimes.Size())
            return gui::TimeMarker{};
        const StringView name = marker < clip->source.eventNames.Size() ? clip->source.eventNames[marker].AsView() : StringView{};
        return gui::TimeMarker{clip->source.eventTimes[marker], name.IsEmpty() ? Format(u8"Event {}", marker + 1) : String(name)};
    }

    void AnimationClipEditorPage::BeginMarkerMove(usize marker)
    {
        pipeline::AnimationClipAsset* clip = Clip();
        if (clip == nullptr || marker >= clip->source.eventTimes.Size())
            return;
        m_markerMoving = static_cast<i32>(marker);
        m_markerFrom = clip->source.eventTimes[marker];
    }

    void AnimationClipEditorPage::PreviewMarkerMove(usize marker, f32 time)
    {
        pipeline::AnimationClipAsset* clip = Clip();
        if (clip == nullptr || static_cast<i32>(marker) != m_markerMoving || marker >= clip->source.eventTimes.Size())
            return;
        // Written into the asset live (the row follows); the undo step is taken when the drag ends.
        clip->source.eventTimes[marker] = Clamp(time, 0.0f, Length());
        m_eventRows->SetValue(gui::JoinPropertyPath(Format(u8"{}.{}", kEvents, marker).AsView(), u8"time").AsView(), 0,
                              gui::Variant(static_cast<f64>(clip->source.eventTimes[marker])));
    }

    void AnimationClipEditorPage::CommitMarkerMove()
    {
        pipeline::AnimationClipAsset* clip = Clip();
        const i32 moving = m_markerMoving;
        m_markerMoving = -1;
        if (clip == nullptr || moving < 0 || static_cast<usize>(moving) >= clip->source.eventTimes.Size())
            return;
        const usize at = static_cast<usize>(moving);
        const f32 time = clip->source.eventTimes[at];
        clip->source.eventTimes[at] = m_markerFrom; // the undo step's before state is where the drag began
        if (time == m_markerFrom)
            return;
        EditWhole(u8"edit_animclip", [at, time](ISerializable& object)
                  {
                      auto& source = static_cast<pipeline::AnimationClipAsset&>(object).source;
                      Square(source);
                      source.eventTimes[at] = time;
                  });
    }

    void AnimationClipEditorPage::CancelMarkerMove()
    {
        pipeline::AnimationClipAsset* clip = Clip();
        const i32 moving = m_markerMoving;
        m_markerMoving = -1;
        if (clip == nullptr || moving < 0 || static_cast<usize>(moving) >= clip->source.eventTimes.Size())
            return;
        clip->source.eventTimes[static_cast<usize>(moving)] = m_markerFrom;
        ShowRows();
    }

    // === The preview ===

    void AnimationClipEditorPage::LoadPreviewPref()
    {
        if (!m_rig)
            return;
        if (foundation::settings::Settings* store = Context().ProjectEditorSettings())
            if (const ClipPreviewSettings* section = store->Find<ClipPreviewSettings>())
                for (const ClipPreviewPref& p : section->prefs)
                    if (p.asset == InstanceId())
                    {
                        m_rig->SetSkeleton(p.skeleton);
                        m_rig->SetMesh(p.mesh);
                        return;
                    }
        // Never previewed before: use the skeleton its import names, whose bones its tracks address.
        if (const pipeline::AnimationClipAsset* clip = Clip())
            m_rig->SetSkeleton(clip->skeleton);
        m_rig->SetMesh(Guid{});
    }

    void AnimationClipEditorPage::SavePreviewPref()
    {
        foundation::settings::Settings* store = Context().ProjectEditorSettings();
        if (store == nullptr || !m_rig)
            return;
        ClipPreviewSettings& section = store->Section<ClipPreviewSettings>();
        bool found = false;
        for (ClipPreviewPref& p : section.prefs)
            if (p.asset == InstanceId())
            {
                p.skeleton = m_rig->SkeletonId();
                p.mesh = m_rig->MeshId();
                found = true;
            }
        if (!found)
            section.prefs.PushBack(ClipPreviewPref{InstanceId(), m_rig->SkeletonId(), m_rig->MeshId()});
        store->MarkChanged<ClipPreviewSettings>();
        Context().RequestProjectEditorSettingsSave();
    }

    void AnimationClipEditorPage::FramePreview()
    {
        if (!m_pane || m_pane->Viewport() == nullptr)
            return;
        if (m_rig && m_rig->Frame())
            return;
        // Nothing shown yet: frame a character's height, looking from a little above its middle.
        PreviewViewport* preview = m_pane->Viewport();
        preview->Camera().position = Float3{0.0f, 1.4f, 3.2f};
        preview->Camera().LookAt(Float3{0.0f, 0.9f, 0.0f});
    }

    void AnimationClipEditorPage::SetTime(f32 seconds)
    {
        m_time = Clamp(seconds, 0.0f, Length());
        m_moving = true;
        m_axis->SetPlayhead(m_time);
        m_moving = false;
    }

    void AnimationClipEditorPage::SetTravel(bool travel)
    {
        m_travel = travel;
        if (m_travelButton != nullptr)
            m_travelButton->SetChecked(travel);
    }

    void AnimationClipEditorPage::Pose()
    {
        if (!m_rig)
            return;
        foundation::animation::AnimationClip* clip = m_clip ? m_clip.Get() : nullptr;
        foundation::animation::Skeleton* skeleton = m_rig->Skeleton();
        if (m_pane)
        {
            if (skeleton == nullptr)
                m_pane->ShowEmpty(u8"No skeleton to play it on", u8"Pick the skeleton its bones belong to on the preview's bar.");
            else
                m_pane->Show();
        }
        if (clip == nullptr || skeleton == nullptr || skeleton->BoneCount() <= 0)
        {
            m_rig->Show(Span<const foundation::animation::BoneTransform>{});
            return;
        }
        const usize bones = static_cast<usize>(skeleton->BoneCount());
        m_pose.Resize(bones);
        foundation::animation::SampleClip(*clip, *skeleton, m_time, Span<foundation::animation::BoneTransform>{m_pose.Data(), bones});
        // Placed under the armature's rest transform, as the model places it (the turn a Z-up file
        // needs, and its units). The cooked pose plays in place; while travelling, the rig goes where
        // its root motion takes it.
        const Float4x4 rest = Clip() != nullptr ? Clip()->modelRest.ToMatrix() : Float4x4::Identity();
        const Float4x4 base = m_travel ? rest * RootMotionPlacement(*clip, m_time) : rest;
        m_rig->Show(m_pose.AsSpan(), base);
        if (m_pane && m_pane->Viewport() != nullptr && m_pane->Viewport()->IsValid())
            DrawRootMotionPath(m_pane->Viewport()->SceneDebugDraw(), *clip, m_time);
    }

    void AnimationClipEditorPage::FollowAxis()
    {
        // The ruler shows the whole clip, fitted again when its width or the clip's length changes.
        const f32 length = Length();
        if (m_axis->Duration() != length)
            m_axis->SetDuration(length);
        const f32 width = m_ruler != nullptr ? m_ruler->GetContentBounds().width : 0.0f;
        if (width > 0.0f && (width != m_rulerWidth || length != m_fittedLength))
        {
            m_rulerWidth = width;
            m_fittedLength = length;
            m_axis->Frame(0.0f, length, width);
        }
    }

    void AnimationClipEditorPage::ShowTransport()
    {
        if (m_state == nullptr)
            return;
        const bool cooked = m_clip && m_clip.Get() != nullptr;
        // The style sheet fixes its width. Its colour shows when the clock is paused, as the Play
        // button's icon does.
        const String text = !cooked ? String(u8"Not cooked yet") : Format(u8"{} / {}", Seconds(m_time), Seconds(Length()));
        if (m_state->GetText() != text.AsView())
            m_state->SetText(text.AsView());
        if (m_playing)
        {
            m_state->AddClass(u8"playing");
            m_state->RemoveClass(u8"paused");
        }
        else
        {
            m_state->AddClass(u8"paused");
            m_state->RemoveClass(u8"playing");
        }
    }

    // === The transport: it drives only the preview ===

    bool AnimationClipEditorPage::CanPlay() const
    {
        return m_clip && m_clip.Get() != nullptr;
    }

    void AnimationClipEditorPage::Play()
    {
        // Resumes; a clip that ran to its end starts over.
        if (m_time >= Length())
            SetTime(0.0f);
        m_playing = true;
    }

    void AnimationClipEditorPage::Stop()
    {
        m_playing = false;
        SetTime(0.0f);
    }

    void AnimationClipEditorPage::Restart()
    {
        SetTime(0.0f);
        m_playing = true;
    }

    void AnimationClipEditorPage::OnUpdate(foundation::runtime::IApplicationHost& host, f32 dt)
    {
        app::AssetObjectPage::OnUpdate(host, dt);
        FollowAxis();
        if (m_playing && CanPlay())
        {
            const foundation::animation::AnimationClip* clip = m_clip.Get();
            const f32 length = Length();
            f32 time = m_time + dt;
            if (time > length)
            {
                if (clip->isLooping)
                    time = std::fmod(time, length);
                else
                {
                    time = length;
                    m_playing = false;
                }
            }
            SetTime(time);
        }
        ShowTransport();
        Pose();
        if (m_pane)
            m_pane->Update(dt);
    }

    void AnimationClipEditorPage::OnRenderWindow(foundation::runtime::IApplicationHost&, foundation::graphics::FrameContext& frame)
    {
        if (m_pane)
            m_pane->Render(frame);
    }

    void AnimationClipEditorPage::OnClose()
    {
        if (m_pane)
            m_pane->Close();
    }

    // === The factory ===

    const TypeInfo* AnimationClipPageFactory::PrimaryType() const
    {
        return &pipeline::AnimationClipAsset::StaticType();
    }

    UniquePtr<EditorPage> AnimationClipPageFactory::CreatePage(EditorContext& context, foundation::content::Instance& instance)
    {
        auto* page = editor::EditorRootAllocator().New<AnimationClipEditorPage>(context, m_services->Make(), instance);
        return UniquePtr<EditorPage>(page, editor::EditorRootAllocator());
    }

    void RegisterAnimationClipEditor(EditorContext& context, const app::AssetPageServicesSource& services)
    {
        // The settings section for preview choices, registered before the app reads the per-project
        // store.
        GlobalTypeRegistry().Register(ClipPreviewSettings::StaticType(), TypeDomain(u8"Editor"));
        RegisterSerializable<ClipPreviewSettings>();
        context.Pages().Register(UniquePtr<IEditorPageFactory>(editor::EditorRootAllocator().New<AnimationClipPageFactory>(services),
                                                               editor::EditorRootAllocator()));
    }

    RTTI_DEFINE_OBJECT_VERSIONED(ClipPreviewSettings, "rtti::editor::editor.clip", 1)
}
