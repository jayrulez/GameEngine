// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The property animation panel's UI-free half: the track seeds; the clip document over a scratch
// project (its own history, its unsaved mark following the history back to where it was saved, a
// gesture as one step or nothing); the clip as the dopesheet's and curve view's source (its channels
// and paths, keys moved, deleted and inserted, each as one step); the preview (the entity posed and
// its values restored, never a step in any history); and the transport's clock.

#include <doctest/doctest.h>
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

import foundation.core;
import foundation.content;
import foundation.scene;
import foundation.propertyanimation;
import foundation.propertyanimation.resource;
import propertyanimation.pipeline;
import foundation.gui;
import foundation.gui.toolkit;
import editor.core;
import editor.propertyanimation;

using namespace foundation::core;
using namespace editor;
namespace propanim = foundation::propertyanimation;
namespace scene = foundation::scene;
namespace gui = foundation::gui;

// A plain value component (REFLECT_VALUE patches TypeOf<PreviewComp>(), which its manager names).
struct PreviewComp
{
    Float3 position{0.0f, 0.0f, 0.0f};
};

REFLECT_VALUE(PreviewComp, "rtti::propanim::cliptest")
{
    builder.Property<&PreviewComp::position>("position");
}

namespace
{
    // A nested reflected struct (the light.tint path) inside a component that has every animatable
    // kind and one leaf no track takes.
    class TestLight : public Object
    {
        RTTI_OBJECT(TestLight, Object)
    public:
        Color tint{1.0f, 1.0f, 1.0f, 1.0f};
    };
    class TestComp : public Object
    {
        RTTI_OBJECT(TestComp, Object)
    public:
        Float3 position{0.0f, 0.0f, 0.0f};
        Quaternion rotation = Quaternion::Identity;
        f32 intensity = 0.0f;
        i32 flags = 0;
        TestLight light;
    };

    void Register()
    {
        static bool done = false;
        if (done)
            return;
        done = true;
        RegisterCoreTypes();
        RttiRegisterValue_PreviewComp();
        GlobalTypeRegistry().Register(TestLight::StaticType());
        GlobalTypeRegistry().Register(TestComp::StaticType());
        pipeline::RegisterPropertyAnimationAssets();
    }

    CurveKey Key(f32 time, f32 value)
    {
        return CurveKey{time, value, 0.0f, 0.0f, CurveKeyInterpolation::Linear, CurveTangentMode::Broken};
    }

    propanim::PropertyTrack Float3Track(StringView component, f32 x, f32 y, f32 z)
    {
        propanim::PropertyTrack track;
        track.componentType = String(component);
        track.propertyPath = String(u8"position");
        track.kind = propanim::TrackValueKind::Float3;
        track.channels[0].AddKey(Key(0.0f, x));
        track.channels[1].AddKey(Key(0.0f, y));
        track.channels[2].AddKey(Key(0.0f, z));
        return track;
    }

    // A scratch project holding one clip, "Bounce", read into a document.
    struct Bed
    {
        String dir = String(u8"scratch_clip_document_project");
        UniquePtr<EditorProject> project;
        EditorContext context{DefaultAllocator()};
        UniquePtr<ClipDocument> document;
        Guid clip;
        usize changes = 0;

        Bed()
        {
            Register();
            (void)RemoveDirectoryRecursive(dir.AsView());
            REQUIRE(EditorProject::Create(DefaultAllocator(), dir.AsView(), u8"P").IsOk());
            project = EditorProject::Open(DefaultAllocator(), dir.AsView());
            REQUIRE(static_cast<bool>(project));
            context.SetProject(project.Get());
            foundation::content::Instance* instance = pipeline::CreatePropertyAnimationClip(project->SourceDb().RootGroup(), u8"Bounce");
            REQUIRE(instance != nullptr);
            clip = instance->Id();
            document = MakeUnique<ClipDocument>(DefaultAllocator(), context);
            document->OnChanged = [this]() { ++changes; };
            REQUIRE(document->Load(clip));
        }
        ~Bed()
        {
            document.Reset();
            context.SetProject(nullptr);
            project.Reset();
            (void)RemoveDirectoryRecursive(dir.AsView());
        }
        void AddTrack(propanim::PropertyTrack track)
        {
            document->Edit(u8"test", [&track](propanim::PropertyAnimationClip& edited) { edited.tracks.PushBack(track); });
        }
    };
}

REFLECT_MEMBERS(TestLight, "rtti::propanim::cliptest") { builder.Property<&TestLight::tint>("tint"); }

REFLECT_MEMBERS(TestComp, "rtti::propanim::cliptest")
{
    builder.Property<&TestComp::position>("position")
        .Property<&TestComp::rotation>("rotation")
        .Property<&TestComp::intensity>("intensity")
        .Property<&TestComp::flags>("flags")
        .Nested<&TestComp::light>("light");
}

TEST_CASE("clip seeds: the four animatable kinds, nested leaves by their paths, nothing else")
{
    Register();
    CHECK(InferTrackKind(&TypeOf<f32>()).Value() == propanim::TrackValueKind::Float);
    CHECK(InferTrackKind(&TypeOf<Float3>()).Value() == propanim::TrackValueKind::Float3);
    CHECK(InferTrackKind(&TypeOf<Color>()).Value() == propanim::TrackValueKind::Color);
    CHECK(InferTrackKind(&TypeOf<Quaternion>()).Value() == propanim::TrackValueKind::Quat);
    CHECK_FALSE(InferTrackKind(&TypeOf<i32>()).HasValue());
    CHECK_FALSE(InferTrackKind(nullptr).HasValue());

    Array<AnimatablePropertyInfo> out;
    CollectAnimatableProperties(TestComp::StaticType(), out);
    REQUIRE(out.Size() == 4u); // position, rotation, intensity, light.tint: not flags
    bool tint = false;
    for (const AnimatablePropertyInfo& seed : out)
        tint = tint || (seed.propertyPath == String(u8"light.tint") && seed.kind == propanim::TrackValueKind::Color);
    CHECK(tint);

    // An entity's seeds: its transform's three first, then its components'.
    scene::Scene sc(DefaultAllocator(), u8"seeds");
    auto* manager = sc.AddSystem<scene::ComponentManager<PreviewComp>>();
    const scene::EntityHandle e = sc.CreateEntity(u8"e");
    (void)manager->Add(e);
    ClipPreview preview(sc);
    const Array<AnimatablePropertyInfo> seeds = preview.TrackSeeds(sc.GetEntityId(e));
    REQUIRE(seeds.Size() == 4u);
    CHECK(seeds[0].componentType == String(u8"Transform"));
    CHECK(seeds[1].kind == propanim::TrackValueKind::Quat);
    CHECK(seeds[3].componentType == String(u8"PreviewComp"));
    CHECK(preview.TrackSeeds(Guid{}).IsEmpty());
}

TEST_CASE("clip document: its own history, unsaved until back where it was saved; a gesture one step or nothing")
{
    Bed bed;
    CHECK(bed.document->HasClip());
    CHECK(bed.document->Name() == StringView(u8"Bounce"));
    CHECK_FALSE(bed.document->IsDirty());

    bed.AddTrack(Float3Track(u8"PreviewComp", 1.0f, 2.0f, 3.0f));
    CHECK(bed.document->Clip().tracks.Size() == 1u);
    CHECK(bed.document->IsDirty());
    bed.document->Commands().Undo();
    CHECK(bed.document->Clip().tracks.IsEmpty());
    CHECK_FALSE(bed.document->IsDirty()); // back where it was loaded
    bed.document->Commands().Redo();
    CHECK(bed.document->IsDirty());

    // Saved, then read back: the track is kept.
    REQUIRE(bed.document->Save().IsOk());
    CHECK_FALSE(bed.document->IsDirty());
    ClipDocument again(bed.context);
    REQUIRE(again.Load(bed.clip));
    REQUIRE(again.Clip().tracks.Size() == 1u);
    CHECK(again.Clip().tracks[0].channels[1].Evaluate(0.0f) == doctest::Approx(2.0f));

    // A new step below the save takes the save off the history, so the clip is unsaved even back
    // there.
    bed.document->Commands().Undo();
    CHECK(bed.document->IsDirty());
    bed.AddTrack(Float3Track(u8"PreviewComp", 0.0f, 0.0f, 0.0f));
    bed.document->Commands().Undo();
    CHECK(bed.document->IsDirty());

    // A gesture edits live, then becomes one step; a cancelled one changes nothing.
    bed.document->Commands().Redo();
    const usize steps = bed.document->Commands().Size();
    bed.document->BeginGesture();
    bed.document->GestureClip().tracks[0].channels[0].Keys()[0].value = 9.0f;
    bed.document->PreviewGesture();
    CHECK(bed.document->Clip().tracks[0].channels[0].Evaluate(0.0f) == doctest::Approx(9.0f));
    bed.document->CommitGesture(u8"test");
    CHECK(bed.document->Commands().Size() == steps + 1u);
    bed.document->BeginGesture();
    bed.document->GestureClip().tracks[0].channels[0].Keys()[0].value = 4.0f;
    bed.document->CancelGesture();
    CHECK(bed.document->Clip().tracks[0].channels[0].Evaluate(0.0f) == doctest::Approx(9.0f));
    CHECK(bed.document->Commands().Size() == steps + 1u);

    // The length covers the keys.
    bed.document->Edit(u8"test", [](propanim::PropertyAnimationClip& clip) { clip.tracks[0].channels[0].AddKey(Key(3.0f, 1.0f)); });
    CHECK(bed.document->Length() == doctest::Approx(3.0f));
    CHECK(bed.changes > 0u);

    bed.document->Close();
    CHECK_FALSE(bed.document->HasClip());
    CHECK_FALSE(bed.document->IsDirty());
}

TEST_CASE("clip source: a channel per curve and per rotation, keys moved, deleted and inserted, each one step")
{
    Bed bed;
    ClipSource source(*bed.document);
    bed.document->OnChanged = [&source]() { source.Refresh(); };
    bed.AddTrack(Float3Track(u8"Transform", 1.0f, 2.0f, 3.0f));
    propanim::PropertyTrack spin;
    spin.componentType = String(u8"Transform");
    spin.propertyPath = String(u8"rotation");
    spin.kind = propanim::TrackValueKind::Quat;
    spin.quatKeys.PushBack(propanim::QuatKey{0.0f, Quaternion::Identity});
    spin.quatKeys.PushBack(propanim::QuatKey{1.0f, Quaternion::Identity});
    bed.AddTrack(spin);

    REQUIRE(source.ChannelCount() == 4u);
    CHECK(source.GetChannelPath(0) == String(u8"Transform.position.x"));
    CHECK(source.GetChannelPath(2) == String(u8"Transform.position.z"));
    CHECK(source.GetChannelPath(3) == String(u8"Transform.rotation"));
    Array<f32> times;
    source.GetKeyTimes(3, times);
    REQUIRE(times.Size() == 2u);
    CHECK(times[1] == doctest::Approx(1.0f));

    // A move is previewed from where the keys were, then becomes one step; the key's new place is
    // reported.
    bed.AddTrack(Float3Track(u8"PreviewComp", 0.0f, 0.0f, 0.0f)); // channels 4..6
    bed.document->Edit(u8"test", [](propanim::PropertyAnimationClip& clip) { clip.tracks[2].channels[0].AddKey(Key(1.0f, 5.0f)); });
    const usize steps = bed.document->Commands().Size();
    source.BeginKeysMove();
    const gui::TimelineKeyRef first{4, 0};
    Array<gui::TimelineKeyRef> moved;
    source.PreviewKeysMove(Span<const gui::TimelineKeyRef>(&first, 1u), 2.0f, moved);
    REQUIRE(moved.Size() == 1u);
    CHECK(moved[0].Index == 1u); // past the key at 1 s
    source.PreviewKeysMove(Span<const gui::TimelineKeyRef>(&first, 1u), 0.5f, moved);
    CHECK(moved[0].Index == 0u); // each preview starts from the original place
    source.CommitKeysMove();
    CHECK(bed.document->Commands().Size() == steps + 1u);
    source.GetKeyTimes(4, times);
    CHECK(times[0] == doctest::Approx(0.5f));
    bed.document->Commands().Undo();
    source.GetKeyTimes(4, times);
    CHECK(times[0] == doctest::Approx(0.0f));

    // Deleting is one step; with no key there, nothing changes.
    const gui::TimelineKeyRef rotationFirst{3, 0};
    REQUIRE(source.DeleteKeys(Span<const gui::TimelineKeyRef>(&rotationFirst, 1u)));
    CHECK(bed.document->Clip().tracks[1].quatKeys.Size() == 1u);
    const gui::TimelineKeyRef nowhere{3, 9};
    CHECK_FALSE(source.DeleteKeys(Span<const gui::TimelineKeyRef>(&nowhere, 1u)));

    // Inserting takes the clip's value at that time; on an empty channel, the application's.
    gui::TimelineKeyRef made;
    REQUIRE(source.InsertKey(4, 0.5f, made));
    CHECK(made.Index == 1u);
    CHECK(bed.document->Clip().tracks[2].channels[0].Keys()[1].value == doctest::Approx(2.5f)); // halfway from 0 to 5
    source.CurrentValue = [](const propanim::PropertyTrack&) { return Variant::From(Float3{7.0f, 8.0f, 9.0f}); };
    bed.document->Edit(u8"test", [](propanim::PropertyAnimationClip& clip) { clip.tracks[2].channels[1].Clear(); });
    REQUIRE(source.InsertKey(5, 1.0f, made));
    CHECK(bed.document->Clip().tracks[2].channels[1].Keys()[0].value == doctest::Approx(8.0f));

    // The curve view: the shown tracks' scalar curves, never a rotation's.
    const usize shown[] = {0, 1};
    source.ShowTracks(Span<const usize>(shown, 2u));
    REQUIRE(source.CurveCount() == 3u);
    CHECK(source.GetCurveInfo(1).Name == String(u8"Transform.position.y"));
    source.BeginKeysEdit();
    const CurveKey dragged[] = {Key(0.0f, 4.0f)};
    source.PreviewKeys(1, Span<const CurveKey>(dragged, 1u));
    source.CommitKeysEdit();
    CHECK(bed.document->Clip().tracks[0].channels[1].Evaluate(0.0f) == doctest::Approx(4.0f));
    bed.document->Commands().Undo();
    CHECK(bed.document->Clip().tracks[0].channels[1].Evaluate(0.0f) == doctest::Approx(2.0f));
}

TEST_CASE("clip preview: the entity posed, what it held put back, kept again when the tracks change")
{
    Register();
    scene::Scene sc(DefaultAllocator(), u8"preview");
    auto* manager = sc.AddSystem<scene::ComponentManager<PreviewComp>>();
    const scene::EntityHandle e = sc.CreateEntity(u8"e");
    manager->Add(e).position = Float3{5.0f, 6.0f, 7.0f};
    sc.SetLocalPosition(e, Float3{1.0f, 1.0f, 1.0f});
    const Guid id = sc.GetEntityId(e);

    propanim::PropertyAnimationClip clip;
    clip.tracks.PushBack(Float3Track(u8"PreviewComp", 100.0f, 200.0f, 300.0f));
    ClipPreview preview(sc);
    preview.Pose(id, clip, 0.5f);
    CHECK(preview.IsPosing());
    CHECK(manager->Get(e)->position.y == doctest::Approx(200.0f));
    CHECK(preview.Read(id, u8"PreviewComp", u8"position").Get<Float3>().z == doctest::Approx(300.0f));

    // A transform track added: the preview saves and restores the transform too.
    clip.tracks.PushBack(Float3Track(u8"Transform", 9.0f, 9.0f, 9.0f));
    preview.Pose(id, clip, 0.5f);
    CHECK(sc.GetLocalTransform(e).position.x == doctest::Approx(9.0f));
    preview.Stop();
    CHECK_FALSE(preview.IsPosing());
    CHECK(manager->Get(e)->position.x == doctest::Approx(5.0f));
    CHECK(sc.GetLocalTransform(e).position.x == doctest::Approx(1.0f));

    // No such entity: nothing posed.
    preview.Pose(Guid{}, clip, 0.0f);
    CHECK_FALSE(preview.IsPosing());
}

TEST_CASE("clip transport: plays from where it stands, wraps when looping, else stops at the end")
{
    ClipTransport transport;
    CHECK_FALSE(transport.Advance(0.1f, 2.0f)); // stopped
    transport.Play(2.0f);
    CHECK(transport.IsPlaying());
    CHECK(transport.Advance(1.5f, 2.0f));
    CHECK(transport.Advance(1.0f, 2.0f));
    CHECK(transport.Time() == doctest::Approx(0.5f)); // wrapped
    transport.Pause();
    CHECK(transport.IsPaused());
    CHECK_FALSE(transport.Advance(1.0f, 2.0f));
    transport.SetLooping(false);
    transport.Play(2.0f);
    CHECK(transport.Advance(3.0f, 2.0f));
    CHECK(transport.Time() == doctest::Approx(2.0f));
    CHECK_FALSE(transport.IsPlaying());
    transport.Play(2.0f); // at the end: from the start
    CHECK(transport.Time() == doctest::Approx(0.0f));
    transport.Stop();
    CHECK_FALSE(transport.IsPlaying());
}
