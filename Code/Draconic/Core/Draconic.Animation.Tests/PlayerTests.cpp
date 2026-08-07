// AnimationPlayer: playback, event firing, looping, evaluation. Ports the player section of
// Sedulous.Animation.Tests.AnimationEventTests + adds playback/eval coverage.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"

import draconic.foundation;
import draconic.animation;

using namespace draconic::foundation;
using namespace draconic::animation;

// A 1-bone skeleton with a position clip moving +Y over 1s.
static void SetupBone(Skeleton& s)
{
    s.Bones()[0].index = 0;
    s.Bones()[0].parentIndex = -1;
    s.FindRootBones();
    s.BuildChildIndices();
    s.ComputeInverseBindPoses();
}

TEST_CASE("player: event fires when time crosses (single update)")
{
    Skeleton skel{2};
    SetupBone(skel);
    AnimationPlayer player{skel};
    int fireCount = 0;
    player.SetEventHandler(AnimationEventHandler{[&](StringView, f32) { ++fireCount; }});

    AnimationClip clip{u8"Test", 1.0f};
    clip.AddEvent(0.5f, u8"Hit");
    player.Play(&clip);
    player.Update(0.6f);
    CHECK(fireCount == 1);
}

TEST_CASE("player: events fire in order across updates")
{
    Skeleton skel{2};
    SetupBone(skel);
    AnimationPlayer player{skel};
    Array<StringView> fired;
    player.SetEventHandler(AnimationEventHandler{[&](StringView n, f32) { fired.PushBack(n); }});

    AnimationClip clip{u8"Test", 2.0f};
    clip.AddEvent(0.3f, u8"A");
    clip.AddEvent(0.8f, u8"B");
    clip.AddEvent(1.5f, u8"C");
    clip.SortEvents();
    player.Play(&clip);
    player.Update(0.5f);
    CHECK(fired.Size() == 1);
    CHECK(fired[0] == StringView{u8"A"});
    player.Update(0.5f);
    CHECK(fired.Size() == 2);
    CHECK(fired[1] == StringView{u8"B"});
    player.Update(0.5f);
    CHECK(fired.Size() == 3);
    CHECK(fired[2] == StringView{u8"C"});
}

TEST_CASE("player: looping clip fires event each loop + wraps time")
{
    Skeleton skel{2};
    SetupBone(skel);
    AnimationPlayer player{skel};
    int fireCount = 0;
    player.SetEventHandler(AnimationEventHandler{[&](StringView, f32) { ++fireCount; }});

    AnimationClip clip{u8"Test", 1.0f, /*looping*/ true};
    clip.AddEvent(0.5f, u8"Hit");
    player.Play(&clip);
    player.Update(0.8f);
    CHECK(fireCount == 1);
    player.Update(0.8f);
    CHECK(fireCount == 2);
    CHECK(player.CurrentTime() < 1.0f);              // wrapped
    CHECK(player.State() == PlaybackState::Playing); // looping never stops
}

TEST_CASE("player: non-looping clip clamps + stops at the end")
{
    Skeleton skel{2};
    SetupBone(skel);
    AnimationPlayer player{skel};
    AnimationClip clip{u8"Test", 1.0f, /*looping*/ false};
    player.Play(&clip);
    player.Update(2.0f);
    CHECK(NearlyEqual(player.CurrentTime(), 1.0f));
    CHECK(player.State() == PlaybackState::Stopped);
}

TEST_CASE("player: SetEventHandler replaces the old handler")
{
    Skeleton skel{2};
    SetupBone(skel);
    AnimationPlayer player{skel};
    int count1 = 0, count2 = 0;
    AnimationClip clip{u8"Test", 1.0f};
    clip.AddEvent(0.5f, u8"Hit");

    player.SetEventHandler(AnimationEventHandler{[&](StringView, f32) { ++count1; }});
    player.Play(&clip);
    player.Update(0.6f);
    CHECK(count1 == 1);
    CHECK(count2 == 0);

    player.SetEventHandler(AnimationEventHandler{[&](StringView, f32) { ++count2; }});
    player.Play(&clip);
    player.Update(0.6f);
    CHECK(count1 == 1);
    CHECK(count2 == 1);
}

TEST_CASE("player: Evaluate drives skinning matrices from the clip")
{
    Skeleton skel{1};
    skel.Bones()[0].index = 0;
    skel.Bones()[0].parentIndex = -1;
    skel.FindRootBones();
    skel.BuildChildIndices();
    skel.ComputeInverseBindPoses();

    AnimationClip clip{u8"move", 1.0f};
    AnimationClip::Vec3Track* pos = clip.GetOrCreatePositionTrack(0);
    pos->AddKeyframe(0.0f, Float3{0, 0, 0});
    pos->AddKeyframe(1.0f, Float3{0, 10, 0});

    AnimationPlayer player{skel};
    player.Play(&clip);
    player.SetCurrentTime(1.0f);
    Span<const Float4x4> skin = player.GetSkinningMatrices();
    // At t=1 the bone translated +10 in Y from its (identity) bind pose -> skin matrix has Ty=10.
    CHECK(skin.Size() == 1);
    CHECK(NearlyEqual(skin[0].m[3][1], 10.0f));
}
