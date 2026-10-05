// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Root motion's cook (root-motion.md P0): a root that walks bakes its travel into the clip's curve
// and plays in place; what is not extracted (a walk's bob) stays in the pose; a turn bakes as yaw
// and is stripped; a step or cubic root keeps its shape through the fixed bake rate; the armature's
// own channels (model tracks, bone -1) bake through the armature's rest and never reach the pose;
// a text clip from before root motion reads with it off.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

#include <cmath>
#include <initializer_list>

import foundation.core;
import foundation.xml;
import foundation.xml.serialization;
import foundation.animation;
import foundation.animation.resource;

using namespace foundation::core;
using namespace foundation::animation;
using Kind = AnimationClipSource::TrackKind;

namespace
{
    void AddTrack(AnimationClipSource& clip, i32 bone, Kind kind, Span<const f32> times, Span<const Float4> values,
                  InterpolationMode interp = InterpolationMode::Linear)
    {
        clip.trackBone.PushBack(bone);
        clip.trackKind.PushBack(static_cast<u8>(kind));
        clip.trackInterp.PushBack(static_cast<u8>(interp));
        clip.trackStart.PushBack(static_cast<u32>(clip.keyTimes.Size()));
        clip.trackCount.PushBack(static_cast<u32>(times.Size()));
        for (usize i = 0; i < times.Size(); ++i)
        {
            clip.keyTimes.PushBack(times[i]);
            clip.keyValues.PushBack(values[i]);
        }
    }

    // A one-second loop: the hips (bone 0) walk 2 m forward with a bob, and a spine (bone 1) sways.
    void Walk(AnimationClipSource& clip)
    {
        clip.name = String(u8"Walk");
        clip.duration = 1.0f;
        clip.isLooping = false; // sampled at its end, not wrapped back to its start
        const f32 t[] = {0.0f, 0.25f, 0.5f, 0.75f, 1.0f};
        const Float4 hips[] = {{0, 1.0f, 0, 0}, {0, 1.1f, 0.5f, 0}, {0, 1.0f, 1.0f, 0}, {0, 1.1f, 1.5f, 0}, {0, 1.0f, 2.0f, 0}};
        AddTrack(clip, 0, Kind::Position, t, hips);
        const f32 s[] = {0.0f, 1.0f};
        const Float4 sway[] = {{0, 0, 0, 1}, {0, 0.0998f, 0, 0.995f}};
        AddTrack(clip, 1, Kind::Rotation, s, sway);
    }

    BoneTransform Pose(const AnimationClipSource& clip, i32 bone, f32 time)
    {
        AnimationClip runtime;
        REQUIRE(clip.FillClip(runtime));
        BoneTransform poses[3];
        SampleClip(runtime, Skeleton{3}, time, Span<BoneTransform>{poses, 3});
        return poses[bone];
    }
}

TEST_CASE("root motion: a root that walks 2 m bakes a 2 m curve and the pose plays in place")
{
    AnimationClipSource clip;
    Walk(clip);
    clip.rootMotion.horizontal = true;
    REQUIRE(BakeRootMotion(clip, 0));
    REQUIRE_FALSE(clip.rootTimes.IsEmpty());
    CHECK(clip.rootTimes.Size() == clip.rootPositions.Size());
    CHECK(clip.rootTimes[0] == 0.0f);
    CHECK(clip.rootTimes[clip.rootTimes.Size() - 1] == doctest::Approx(1.0f));
    CHECK(clip.rootPositions[clip.rootPositions.Size() - 1].z - clip.rootPositions[0].z == doctest::Approx(2.0f));
    CHECK(clip.rootTimes.Size() >= 31u); // the 30 Hz bake, not only the five keys

    for (const f32 t : {0.0f, 0.3f, 0.5f, 0.9f})
    {
        CHECK(Pose(clip, 0, t).position.z == doctest::Approx(0.0f)); // in place
    }
    // Vertical off: the bob is the pose's still.
    CHECK(Pose(clip, 0, 0.25f).position.y == doctest::Approx(1.1f));
    CHECK(Pose(clip, 0, 0.5f).position.y == doctest::Approx(1.0f));
    // Another bone is untouched.
    CHECK(Pose(clip, 1, 1.0f).rotation.y == doctest::Approx(0.0998f));

    // The runtime clip carries the curve and what it extracted.
    AnimationClip runtime;
    REQUIRE(clip.FillClip(runtime));
    CHECK(runtime.rootMotion.horizontal);
    CHECK_FALSE(runtime.rootMotion.vertical);
    CHECK(runtime.rootMotion.times.Size() == clip.rootTimes.Size());

    // Vertical on: the height is the curve's as well, held at frame 0's in the pose.
    AnimationClipSource climb;
    Walk(climb);
    climb.rootMotion.vertical = true;
    REQUIRE(BakeRootMotion(climb, 0));
    CHECK(Pose(climb, 0, 0.25f).position.y == doctest::Approx(1.0f));
    CHECK(Pose(climb, 0, 0.25f).position.z == doctest::Approx(0.5f)); // horizontal off: still travels
}

TEST_CASE("root motion: a turn bakes as yaw past half a circle and is stripped from the pose")
{
    AnimationClipSource clip;
    clip.duration = 2.0f;
    const f32 t[] = {0.0f, 1.0f, 2.0f};
    const Quaternion a = Quaternion::FromAxisAngle(Float3{0, 1, 0}, 0.0f);
    const Quaternion b = Quaternion::FromAxisAngle(Float3{0, 1, 0}, 2.0f);
    const Quaternion c = Quaternion::FromAxisAngle(Float3{0, 1, 0}, 4.0f); // past pi
    const Float4 keys[] = {{a.x, a.y, a.z, a.w}, {b.x, b.y, b.z, b.w}, {c.x, c.y, c.z, c.w}};
    AddTrack(clip, 0, Kind::Rotation, t, keys);
    clip.rootMotion.yaw = true;
    REQUIRE(BakeRootMotion(clip, 0));
    CHECK(clip.rootYaws[clip.rootYaws.Size() - 1] == doctest::Approx(4.0f).epsilon(1e-3)); // unwrapped
    for (const f32 at : {0.5f, 1.0f, 2.0f})
    {
        const Float3 forward = RotateVector(Pose(clip, 0, at).rotation, Float3{0, 0, 1});
        CHECK(forward.z == doctest::Approx(1.0f).epsilon(1e-4)); // faces where frame 0 did
    }
}

TEST_CASE("root motion: a step root keeps its shape through the fixed bake rate")
{
    AnimationClipSource clip;
    clip.duration = 1.0f;
    const f32 t[] = {0.0f, 0.5f};
    const Float4 hop[] = {{0, 0, 0, 0}, {0, 0, 1, 0}};
    AddTrack(clip, 0, Kind::Position, t, hop, InterpolationMode::Step);
    clip.rootMotion.horizontal = true;
    REQUIRE(BakeRootMotion(clip, 0));
    // Just before the step the curve still stands at 0; at and after it, at 1.
    for (usize i = 0; i < clip.rootTimes.Size(); ++i)
    {
        CHECK(clip.rootPositions[i].z == doctest::Approx(clip.rootTimes[i] < 0.5f ? 0.0f : 1.0f));
    }
}

TEST_CASE("root motion: the armature's own channels bake through its rest and never reach the pose")
{
    // The armature moves 3 m along its parent's +X; at rest it is turned a quarter about +Y, so in
    // model space (the armature at rest) that travel is +X turned by the rest's inverse.
    AnimationClipSource clip;
    Walk(clip);
    const f32 t[] = {0.0f, 1.0f};
    const Float4 moved[] = {{0, 0, 0, 0}, {3, 0, 0, 0}};
    AddTrack(clip, -1, Kind::Position, t, moved);
    const Quaternion turn = Quaternion::FromAxisAngle(Float3{0, 1, 0}, kHalfPi);
    const Float4 still[] = {{turn.x, turn.y, turn.z, turn.w}, {turn.x, turn.y, turn.z, turn.w}};
    AddTrack(clip, -1, Kind::Rotation, t, still);
    clip.rootMotion.horizontal = true;
    Transform rest;
    rest.rotation = turn;
    REQUIRE(BakeRootMotion(clip, -1, rest));
    const Float3 travel = clip.rootPositions[clip.rootPositions.Size() - 1] - clip.rootPositions[0];
    CHECK(Length(travel) == doctest::Approx(3.0f).epsilon(1e-4));
    CHECK(Length(travel - RotateVector(Inverse(turn), Float3{3, 0, 0})) < 1.0e-3f);
    CHECK(clip.rootYaws[clip.rootYaws.Size() - 1] == doctest::Approx(0.0f).epsilon(1e-4)); // at rest: no turn
    // The bone tracks were not touched (the hips still walk), and the model tracks reach no bone.
    CHECK(Pose(clip, 0, 1.0f).position.z == doctest::Approx(2.0f));
    AnimationClip runtime;
    REQUIRE(clip.FillClip(runtime));
    for (const auto& track : runtime.PositionTracks())
    {
        CHECK(track->boneIndex >= 0);
    }
}

TEST_CASE("root motion: off leaves the clip as it was; a root with no track says so")
{
    AnimationClipSource clip;
    Walk(clip);
    REQUIRE(BakeRootMotion(clip, 0));
    CHECK(clip.rootTimes.IsEmpty());
    CHECK(Pose(clip, 0, 1.0f).position.z == doctest::Approx(2.0f));
    clip.rootMotion.horizontal = true;
    CHECK_FALSE(BakeRootMotion(clip, 2)); // bone 2 has no track
    CHECK(clip.rootTimes.IsEmpty());
}

TEST_CASE("root motion: a text clip from before root motion reads with it off")
{
    AnimationClipSource walk;
    Walk(walk);
    walk.rootMotion.horizontal = true;
    REQUIRE(BakeRootMotion(walk, 0));
    String text;
    {
        foundation::xml::XmlSerializer writer(DefaultAllocator());
        walk.Serialize(writer);
        writer.GetOutput(text);
    }
    // The same record as a build before root motion wrote it: no rootMotion, no curve.
    const StringView all = text.AsView();
    const StringView needle(u8"<string name=\"rootBone\"");
    usize cut = all.Size();
    for (usize i = 0; i + needle.Size() <= all.Size(); ++i)
    {
        if (all.SubStr(i, needle.Size()) == needle)
        {
            cut = i;
            break;
        }
    }
    REQUIRE(cut < all.Size());
    String old(all.SubStr(0, cut));
    old += StringView(u8"</root>");
    foundation::xml::XmlDocument doc(DefaultAllocator());
    REQUIRE(doc.Parse(old.AsView()) == foundation::xml::XmlResult::Ok);
    foundation::xml::XmlSerializer reader(doc);
    AnimationClipSource loaded;
    loaded.Serialize(reader);
    CHECK(reader.IsOk());
    CHECK_FALSE(loaded.rootMotion.Any());
    CHECK(loaded.rootTimes.IsEmpty());
    CHECK(loaded.trackBone.Size() == 2u); // the rest still reads
    // And the current record reads its settings and curve back.
    foundation::xml::XmlDocument current(DefaultAllocator());
    REQUIRE(current.Parse(text.AsView()) == foundation::xml::XmlResult::Ok);
    foundation::xml::XmlSerializer again(current);
    AnimationClipSource round;
    round.Serialize(again);
    CHECK(again.IsOk());
    CHECK(round.rootMotion.horizontal);
    CHECK(round.rootTimes.Size() == walk.rootTimes.Size());
}
