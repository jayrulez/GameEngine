// AnimationEvent + AnimationClip event storage + FireEvents. Ported from the applicable parts of
// Sedulous.Animation.Tests.AnimationEventTests (Player/ClipStateNode/BlendTree parts land later).
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"

import draconic.foundation;
import draconic.animation;

using namespace draconic::foundation;
using namespace draconic::animation;

TEST_CASE("event: constructor sets time and name")
{
    const AnimationEvent e{0.5f, u8"Footstep"};
    CHECK(e.time == 0.5f);
    CHECK(e.name == StringView{u8"Footstep"});
}

TEST_CASE("clip: AddEvent increases count")
{
    AnimationClip clip{u8"Test", 2.0f};
    CHECK(clip.Events().Size() == 0);
    clip.AddEvent(0.5f, u8"Hit");
    CHECK(clip.Events().Size() == 1);
    clip.AddEvent(1.0f, u8"Sound");
    CHECK(clip.Events().Size() == 2);
}

TEST_CASE("clip: SortEvents sorts by time")
{
    AnimationClip clip{u8"Test", 2.0f};
    clip.AddEvent(1.5f, u8"C");
    clip.AddEvent(0.2f, u8"A");
    clip.AddEvent(0.8f, u8"B");
    clip.SortEvents();
    CHECK(clip.Events()[0].time == 0.2f);
    CHECK(clip.Events()[1].time == 0.8f);
    CHECK(clip.Events()[2].time == 1.5f);
    CHECK(clip.Events()[0].name == StringView{u8"A"});
    CHECK(clip.Events()[2].name == StringView{u8"C"});
}

TEST_CASE("clip: FireEvents crossing behavior")
{
    { // crosses threshold -> fires
        AnimationClip clip{u8"Test", 2.0f};
        clip.AddEvent(0.5f, u8"Hit");
        int count = 0;
        clip.FireEvents(0.0f, 1.0f, [&](StringView, f32) { ++count; });
        CHECK(count == 1);
    }
    { // before threshold -> no fire
        AnimationClip clip{u8"Test", 2.0f};
        clip.AddEvent(1.5f, u8"Hit");
        int count = 0;
        clip.FireEvents(0.0f, 1.0f, [&](StringView, f32) { ++count; });
        CHECK(count == 0);
    }
    { // exact end time fires (inclusive right)
        AnimationClip clip{u8"Test", 1.0f};
        clip.AddEvent(0.5f, u8"Exact");
        int count = 0;
        clip.FireEvents(0.3f, 0.5f, [&](StringView, f32) { ++count; });
        CHECK(count == 1);
    }
}

TEST_CASE("clip: FireEvents in order + loop wrap + non-looping past duration")
{
    { // multiple fire in order
        AnimationClip clip{u8"Test", 2.0f};
        clip.AddEvent(0.3f, u8"A");
        clip.AddEvent(0.7f, u8"B");
        clip.AddEvent(1.2f, u8"C");
        clip.SortEvents();
        Array<StringView> fired;
        clip.FireEvents(0.0f, 1.5f, [&](StringView n, f32) { fired.PushBack(n); });
        CHECK(fired.Size() == 3);
        CHECK(fired[0] == StringView{u8"A"});
        CHECK(fired[1] == StringView{u8"B"});
        CHECK(fired[2] == StringView{u8"C"});
    }
    { // looping wrap: prev 0.9 -> cur 1.3 on a 1.0s loop fires only the early (post-wrap) event
        AnimationClip clip{u8"Test", 1.0f, true};
        clip.AddEvent(0.2f, u8"Early");
        clip.AddEvent(0.8f, u8"Late");
        clip.SortEvents();
        Array<StringView> fired;
        clip.FireEvents(0.9f, 1.3f, [&](StringView n, f32) { fired.PushBack(n); });
        CHECK(fired.Size() == 1);
        CHECK(fired[0] == StringView{u8"Early"});
    }
    { // non-looping past duration fires remaining up to duration
        AnimationClip clip{u8"Test", 1.0f, false};
        clip.AddEvent(0.8f, u8"NearEnd");
        clip.AddEvent(1.0f, u8"AtEnd");
        clip.SortEvents();
        int count = 0;
        clip.FireEvents(0.5f, 1.5f, [&](StringView, f32) { ++count; });
        CHECK(count == 2);
    }
}

TEST_CASE("clip: ComputeDuration from latest keyframe")
{
    AnimationClip clip{u8"Test"};
    AnimationClip::Vec3Track* pos = clip.GetOrCreatePositionTrack(0);
    pos->AddKeyframe(0.0f, Float3{0, 0, 0});
    pos->AddKeyframe(1.25f, Float3{1, 0, 0});
    clip.ComputeDuration();
    CHECK(NearlyEqual(clip.duration, 1.25f));
    // GetOrCreate returns the SAME track for the same bone.
    CHECK(clip.GetOrCreatePositionTrack(0) == pos);
    CHECK(clip.PositionTracks().Size() == 1);
}
