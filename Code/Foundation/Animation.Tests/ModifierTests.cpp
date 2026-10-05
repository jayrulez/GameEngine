// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The pose modifier stage (inverse-kinematics.md P0b): a modifier changes the pose between the
// sample and the palette, every evaluation, so its change survives the next sample and the
// sampled pose stays as sampled; modifiers run in their order over one model-space build; a
// partial rebuild matches a full one; and no modifier changes nothing.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.animation;

using namespace foundation::core;
using namespace foundation::animation;

namespace
{
    // root(0) -> child(1) -> grandchild(2), each +Y from its parent, plus a branch(3) off the root.
    void BuildTree(Skeleton& s)
    {
        Array<Bone>& bones = s.Bones();
        const i32 parents[] = {-1, 0, 1, 0};
        const f32 ys[] = {10.0f, 5.0f, 2.0f, 3.0f};
        for (i32 i = 0; i < 4; ++i)
        {
            bones[static_cast<usize>(i)].index = i;
            bones[static_cast<usize>(i)].parentIndex = parents[i];
            bones[static_cast<usize>(i)].localBindPose.position = Float3{0, ys[i], 0};
        }
        s.BuildNameMap();
        s.FindRootBones();
        s.BuildChildIndices();
        s.ComputeInverseBindPoses();
    }

    // Moves one bone's local position up by `lift` and keeps the model pose current.
    class Lift final : public IPoseModifier
    {
    public:
        Lift(i32 bone, f32 lift, Array<i32>* log = nullptr, i32 tag = 0)
            : m_bone(bone), m_lift(lift), m_log(log), m_tag(tag)
        {
        }
        void Apply(const Skeleton& skeleton, Span<BoneTransform> local, ModelPoseCache& model) override
        {
            if (m_log != nullptr)
            {
                m_log->PushBack(m_tag);
            }
            local[static_cast<usize>(m_bone)].position.y += m_lift;
            model.RebuildFrom(skeleton, Span<const BoneTransform>{local.Data(), local.Size()}, m_bone);
            seenGrandchildY = model.At(2).m[3][1];
        }
        f32 seenGrandchildY = 0.0f;

    private:
        i32 m_bone;
        f32 m_lift;
        Array<i32>* m_log;
        i32 m_tag;
    };

    f32 WorldY(const Skeleton& skeleton, Span<const BoneTransform> pose, i32 bone)
    {
        ModelPoseCache cache;
        cache.Build(skeleton, pose);
        return cache.At(bone).m[3][1];
    }
}

TEST_CASE("pose modifiers: a change survives the next sample, the sampled pose stays as sampled")
{
    Skeleton skel{4};
    BuildTree(skel);
    // The clip moves the child: its y climbs 5 -> 6 over the second.
    AnimationClip clip{u8"Climb", 1.0f, true};
    clip.GetOrCreatePositionTrack(1)->AddKeyframe(0.0f, Float3{0, 5, 0});
    clip.GetOrCreatePositionTrack(1)->AddKeyframe(1.0f, Float3{0, 6, 0});
    AnimationPlayer player{skel};
    Lift lift{2, 1.0f};
    player.Modifiers().Add(&lift);
    player.Play(&clip);

    for (i32 frame = 0; frame < 3; ++frame)
    {
        player.Update(0.25f);
        (void)player.GetSkinningMatrices();
        const f32 sampledChildY = player.GetLocalPoses()[1].position.y;
        CHECK(player.GetLocalPoses()[2].position.y == doctest::Approx(2.0f)); // sampled: untouched
        CHECK(player.GetFinalPoses()[2].position.y == doctest::Approx(3.0f)); // modified, every time
        CHECK(WorldY(skel, player.GetFinalPoses(), 2) == doctest::Approx(10.0f + sampledChildY + 3.0f));
    }
    // A paused player still re-runs its modifiers (a target moves whether or not the clip does).
    player.Pause();
    Lift more{2, 2.0f};
    player.Modifiers().Add(&more);
    (void)player.GetSkinningMatrices();
    CHECK(player.GetFinalPoses()[2].position.y == doctest::Approx(5.0f));
    player.Modifiers().Remove(&more);
    player.Modifiers().Remove(&lift);
}

TEST_CASE("pose modifiers: they run lowest order first, over one model-space build an evaluation")
{
    Skeleton skel{4};
    BuildTree(skel);
    AnimationPlayer player{skel};
    Array<i32> log;
    Lift late{1, 1.0f, &log, 2};
    Lift early{1, 1.0f, &log, 1};
    Lift tie{1, 1.0f, &log, 3};
    player.Modifiers().Add(&late, 5);
    player.Modifiers().Add(&early, -1);
    player.Modifiers().Add(&tie, 5); // equal orders keep the order they were added in
    const u32 before = player.Modifiers().Model().FullBuilds();
    (void)player.GetSkinningMatrices();
    REQUIRE(log.Size() == 3u);
    CHECK(log[0] == 1);
    CHECK(log[1] == 2);
    CHECK(log[2] == 3);
    CHECK(player.Modifiers().Model().FullBuilds() == before + 1);
    // Each saw the pose the ones before it left: the grandchild rose one more each time.
    CHECK(early.seenGrandchildY == doctest::Approx(18.0f));
    CHECK(late.seenGrandchildY == doctest::Approx(19.0f));
    CHECK(tie.seenGrandchildY == doctest::Approx(20.0f));
    player.Modifiers().Add(&early, 0); // already there: not added twice
    CHECK(player.Modifiers().Count() == 3u);
}

TEST_CASE("pose modifiers: a partial rebuild matches a full one and leaves the rest alone")
{
    Skeleton skel{4};
    BuildTree(skel);
    Array<BoneTransform> pose;
    for (i32 i = 0; i < 4; ++i)
    {
        pose.PushBack(skel.GetBone(i)->localBindPose);
    }
    ModelPoseCache cache;
    cache.Build(skel, Span<const BoneTransform>{pose.Data(), pose.Size()});
    const Float4x4 rootBefore = cache.At(0);
    const Float4x4 branchBefore = cache.At(3);
    pose[1].position.y = 8.0f;
    pose[1].rotation = Quaternion::FromAxisAngle(Float3{0, 0, 1}, 0.5f);
    cache.RebuildFrom(skel, Span<const BoneTransform>{pose.Data(), pose.Size()}, 1);
    ModelPoseCache full;
    full.Build(skel, Span<const BoneTransform>{pose.Data(), pose.Size()});
    for (i32 bone = 0; bone < 4; ++bone)
    {
        for (i32 r = 0; r < 4; ++r)
        {
            for (i32 c = 0; c < 4; ++c)
            {
                CHECK(cache.At(bone).m[r][c] == doctest::Approx(full.At(bone).m[r][c]));
            }
        }
    }
    CHECK(cache.At(0).m[3][1] == rootBefore.m[3][1]);   // above the change
    CHECK(cache.At(3).m[3][1] == branchBefore.m[3][1]); // beside it
}

TEST_CASE("pose modifiers: none changes nothing; the graph player runs them after its layers")
{
    Skeleton skel{4};
    BuildTree(skel);
    AnimationClip clip{u8"Climb", 1.0f, true};
    clip.GetOrCreatePositionTrack(1)->AddKeyframe(0.0f, Float3{0, 5, 0});
    clip.GetOrCreatePositionTrack(1)->AddKeyframe(1.0f, Float3{0, 6, 0});
    AnimationPlayer plain{skel};
    AnimationPlayer emptied{skel};
    Lift lift{2, 1.0f};
    emptied.Modifiers().Add(&lift);
    emptied.Modifiers().Remove(&lift);
    plain.Play(&clip);
    emptied.Play(&clip);
    plain.Update(0.4f);
    emptied.Update(0.4f);
    const Span<const Float4x4> a = plain.GetSkinningMatrices();
    const Span<const Float4x4> b = emptied.GetSkinningMatrices();
    REQUIRE(a.Size() == b.Size());
    CHECK(MemCompare(a.Data(), b.Data(), a.Size() * sizeof(Float4x4)) == 0);

    AnimationGraph graph;
    auto layer = MakeUnique<AnimationLayer>(DefaultAllocator(), StringView{u8"Base"});
    layer->AddState(MakeUnique<AnimationGraphState>(DefaultAllocator(), StringView{u8"Climb"},
                                                    MakeUnique<ClipStateNode>(DefaultAllocator(), &clip)));
    graph.AddLayer(static_cast<UniquePtr<AnimationLayer>&&>(layer));
    AnimationGraphPlayer player{graph, skel};
    player.Modifiers().Add(&lift);
    player.Update(0.25f);
    CHECK(player.GetLocalPoses()[2].position.y == doctest::Approx(3.0f));
    player.Update(0.25f); // the layers combine afresh; the modifier applies again, once
    CHECK(player.GetLocalPoses()[2].position.y == doctest::Approx(3.0f));
    player.Modifiers().Remove(&lift);
}
