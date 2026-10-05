// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The inverse kinematics solvers (inverse-kinematics.md P1, the tests the spec lists): a two-bone
// chain reaches a reachable target exactly from every side, keeps its bone lengths and bends in
// the pole's plane; an unreachable target is clamped short of straight with no NaN; a straight
// chain bends the same way every frame; weight 0 is byte for byte and 0.5 between; an aim reaches
// within its limit, clamps beyond it and holds its up axis; bones above a chain never move; and a
// solve allocates nothing once its cache is sized.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

#include <cmath>
#include <cstring>
#include <initializer_list>

import foundation.core;
import foundation.animation;

using namespace foundation::core;
using namespace foundation::animation;

namespace
{
    // Pelvis(0) at y 10; a leg: Thigh(1) at the hip, Shin(2) 4 below, Foot(3) 3 below that (bent
    // forward by `kneeForward` in the bind pose), Toe(4) ahead of the foot; and a spine branch:
    // Spine(5), Neck(6), Head(7), looking down +z.
    enum Leg : i32
    {
        kPelvis = 0,
        kThigh,
        kShin,
        kFoot,
        kToe,
        kSpine,
        kNeck,
        kHead,
        kBoneCount
    };

    void BuildBody(Skeleton& s, f32 kneeForward = 0.0f)
    {
        Array<Bone>& bones = s.Bones();
        const i32 parents[] = {-1, kPelvis, kThigh, kShin, kFoot, kPelvis, kSpine, kNeck};
        const Float3 offsets[] = {{0, 10, 0}, {1, 0, 0}, {0, -4, kneeForward}, {0, -3, -kneeForward},
                                  {0, 0, 1},  {0, 2, 0}, {0, 1, 0},           {0, 0.5f, 0}};
        for (i32 i = 0; i < kBoneCount; ++i)
        {
            bones[static_cast<usize>(i)].index = i;
            bones[static_cast<usize>(i)].parentIndex = parents[i];
            bones[static_cast<usize>(i)].localBindPose.position = offsets[i];
        }
        s.BuildNameMap();
        s.FindRootBones();
        s.BuildChildIndices();
        s.ComputeInverseBindPoses();
    }

    Array<BoneTransform> BindPose(const Skeleton& s)
    {
        Array<BoneTransform> pose;
        for (i32 i = 0; i < s.BoneCount(); ++i)
        {
            pose.PushBack(s.GetBone(i)->localBindPose);
        }
        return pose;
    }

    Span<BoneTransform> All(Array<BoneTransform>& pose) { return {pose.Data(), pose.Size()}; }

    Float3 At(const ModelPoseCache& cache, i32 bone)
    {
        const Float4x4& m = cache.At(bone);
        return Float3{m.m[3][0], m.m[3][1], m.m[3][2]};
    }

    Float3 Axis(const ModelPoseCache& cache, i32 bone, i32 row)
    {
        const Float4x4& m = cache.At(bone);
        return Normalized(Float3{m.m[row][0], m.m[row][1], m.m[row][2]});
    }

    bool Finite(Span<const BoneTransform> pose)
    {
        for (const BoneTransform& t : pose)
        {
            const f32 v[] = {t.position.x, t.position.y, t.position.z, t.rotation.x,
                             t.rotation.y, t.rotation.z, t.rotation.w};
            for (f32 x : v)
            {
                if (!std::isfinite(x))
                {
                    return false;
                }
            }
        }
        return true;
    }

    // The leg as an animation leaves it: the knee bent forward 0.5 rad (the shin turns about x).
    Array<BoneTransform> BentLeg(const Skeleton& s)
    {
        Array<BoneTransform> pose = BindPose(s);
        pose[kShin].rotation = Quaternion::FromAxisAngle(Float3{1, 0, 0}, -0.5f);
        return pose;
    }

    constexpr TwoBoneIkChain kLeg{kThigh, kShin, kFoot};
    constexpr f32 kReach = 7.0f;
}

TEST_CASE("ik two-bone: a reachable target is reached exactly from every side, lengths kept")
{
    Skeleton skel{kBoneCount};
    BuildBody(skel);
    const Float3 hip{1, 10, 0};
    usize solved = 0;
    // Directions spread over the sphere (a golden-angle spiral), at three distances inside reach.
    for (i32 i = 0; i < 64; ++i)
    {
        const f32 y = 1.0f - 2.0f * (static_cast<f32>(i) + 0.5f) / 64.0f;
        const f32 r = std::sqrt(1.0f - y * y);
        const f32 phi = 2.399963f * static_cast<f32>(i);
        const Float3 direction{r * std::cos(phi), y, r * std::sin(phi)};
        for (const f32 distance : {1.5f, 4.0f, 6.9f})
        {
            for (const bool withPole : {false, true})
            {
                Array<BoneTransform> pose = BentLeg(skel);
                ModelPoseCache cache;
                cache.Build(skel, All(pose));
                TwoBoneIkSettings settings;
                settings.target = hip + direction * distance;
                settings.hasPole = withPole;
                settings.pole = hip + Float3{0, -3, 5};
                const IkResult result = SolveTwoBone(skel, All(pose), cache, kLeg, settings);
                REQUIRE(result.valid);
                CHECK(result.reached);
                CHECK(result.error < 1.0e-4f * kReach);
                CHECK(Length(At(cache, kFoot) - settings.target) < 1.0e-4f * kReach);
                CHECK(Length(At(cache, kShin) - At(cache, kThigh)) == doctest::Approx(4.0f).epsilon(1e-5));
                CHECK(Length(At(cache, kFoot) - At(cache, kShin)) == doctest::Approx(3.0f).epsilon(1e-5));
                CHECK(Finite(All(pose)));
                if (withPole)
                {
                    // The knee lies in the plane of hip, target and pole, on the pole's side.
                    const Float3 toTarget = settings.target - hip;
                    const Float3 toPole = settings.pole - hip;
                    const Float3 normal = Cross(toTarget, toPole);
                    if (Length(normal) > 0.1f * Length(toTarget) * Length(toPole))
                    {
                        const Float3 knee = At(cache, kShin) - hip;
                        CHECK(std::abs(Dot(knee, Normalized(normal))) < 1.0e-3f);
                        const Float3 along = Normalized(toTarget);
                        CHECK(Dot(knee - along * Dot(knee, along), toPole - along * Dot(toPole, along)) > 0.0f);
                    }
                }
                ++solved;
            }
        }
    }
    CHECK(solved == 384u);
}

TEST_CASE("ik two-bone: an unreachable target is clamped short of straight, never NaN")
{
    Skeleton skel{kBoneCount};
    BuildBody(skel);
    const Float3 hip{1, 10, 0};

    SUBCASE("too far: pointed at it, stopped at 0.995 of full reach")
    {
        Array<BoneTransform> pose = BentLeg(skel);
        ModelPoseCache cache;
        cache.Build(skel, All(pose));
        TwoBoneIkSettings settings;
        settings.target = hip + Float3{0, 0, 20};
        const IkResult result = SolveTwoBone(skel, All(pose), cache, kLeg, settings);
        CHECK(result.valid);
        CHECK_FALSE(result.reached);
        const Float3 foot = At(cache, kFoot) - hip;
        CHECK(Length(foot) == doctest::Approx(kTwoBoneMaxReach * kReach).epsilon(1e-4));
        CHECK(Dot(Normalized(foot), Float3{0, 0, 1}) > 1.0f - 1.0e-5f);
        CHECK(result.error == doctest::Approx(20.0f - kTwoBoneMaxReach * kReach).epsilon(1e-3));
    }
    SUBCASE("at the chain's start: folded to its shortest, finite")
    {
        Array<BoneTransform> pose = BentLeg(skel);
        ModelPoseCache cache;
        cache.Build(skel, All(pose));
        TwoBoneIkSettings settings;
        settings.target = hip;
        const IkResult result = SolveTwoBone(skel, All(pose), cache, kLeg, settings);
        CHECK(result.valid);
        CHECK(Finite(All(pose)));
        CHECK(Length(At(cache, kFoot) - hip) == doctest::Approx(1.0f).epsilon(1e-3)); // |4 - 3|
    }
    SUBCASE("a straight chain and a target straight behind it: finite, and reached")
    {
        Array<BoneTransform> pose = BindPose(skel); // straight down
        ModelPoseCache cache;
        cache.Build(skel, All(pose));
        TwoBoneIkSettings settings;
        settings.target = hip + Float3{0, 3, 0}; // on the chain's line, the other way
        const IkResult result = SolveTwoBone(skel, All(pose), cache, kLeg, settings);
        CHECK(result.valid);
        CHECK(Finite(All(pose)));
        CHECK(result.reached);
    }
    SUBCASE("bones that do not form a chain change nothing")
    {
        Array<BoneTransform> pose = BentLeg(skel);
        const Array<BoneTransform> before = BentLeg(skel);
        ModelPoseCache cache;
        cache.Build(skel, All(pose));
        TwoBoneIkSettings settings;
        settings.target = hip + Float3{0, -5, 1};
        CHECK_FALSE(SolveTwoBone(skel, All(pose), cache, TwoBoneIkChain{kThigh, kSpine, kHead}, settings).valid);
        CHECK_FALSE(SolveTwoBone(skel, All(pose), cache, TwoBoneIkChain{kThigh, kShin, 99}, settings).valid);
        CHECK(std::memcmp(pose.Data(), before.Data(), sizeof(BoneTransform) * pose.Size()) == 0);
    }
}

TEST_CASE("ik two-bone: a straight chain bends by the bind pose, then the hinge, the same way each frame")
{
    const Float3 hip{1, 10, 0};
    TwoBoneIkSettings settings;
    settings.target = hip + Float3{0, -5, 0}; // straight below: nothing in the target picks a side

    SUBCASE("the bind pose's bend: a knee bound forward bends forward")
    {
        Skeleton skel{kBoneCount};
        BuildBody(skel, 0.3f);
        Array<BoneTransform> pose = BindPose(skel);
        pose[kShin].position = Float3{0, -4, 0}; // the animation holds it straight
        pose[kFoot].position = Float3{0, -3, 0};
        ModelPoseCache cache;
        cache.Build(skel, All(pose));
        CHECK(SolveTwoBone(skel, All(pose), cache, kLeg, settings).reached);
        CHECK(At(cache, kShin).z > 0.5f);
    }
    SUBCASE("no bind bend: the hinge decides")
    {
        Skeleton skel{kBoneCount};
        BuildBody(skel);
        settings.hingeAxis = Float3{1, 0, 0};
        Array<BoneTransform> pose = BindPose(skel);
        ModelPoseCache cache;
        cache.Build(skel, All(pose));
        CHECK(SolveTwoBone(skel, All(pose), cache, kLeg, settings).reached);
        // cross(x, -y) = -z: the mid moves toward cross(hinge, chain direction).
        CHECK(At(cache, kShin).z < -0.5f);
    }
    SUBCASE("nothing to go by: a fixed side, the same on every frame and on the frame after")
    {
        Skeleton skel{kBoneCount};
        BuildBody(skel);
        Array<BoneTransform> first = BindPose(skel);
        Array<BoneTransform> second = BindPose(skel);
        ModelPoseCache a;
        ModelPoseCache b;
        a.Build(skel, All(first));
        b.Build(skel, All(second));
        CHECK(SolveTwoBone(skel, All(first), a, kLeg, settings).reached);
        CHECK(SolveTwoBone(skel, All(second), b, kLeg, settings).reached);
        CHECK(std::memcmp(first.Data(), second.Data(), sizeof(BoneTransform) * first.Size()) == 0);
        // The next frame starts from this bent pose: the knee stays where it went.
        const Float3 knee = At(a, kShin);
        CHECK(SolveTwoBone(skel, All(first), a, kLeg, settings).reached);
        CHECK(Length(At(a, kShin) - knee) < 1.0e-4f);
    }
}

TEST_CASE("ik two-bone: weight 0 leaves the pose byte for byte, 0.5 is between, the end can turn")
{
    Skeleton skel{kBoneCount};
    BuildBody(skel);
    const Float3 hip{1, 10, 0};
    TwoBoneIkSettings settings;
    settings.target = hip + Float3{1, -4, 3};
    settings.pole = hip + Float3{0, 0, 5};
    settings.hasPole = true;

    Array<BoneTransform> untouched = BentLeg(skel);
    const Array<BoneTransform> original = BentLeg(skel);
    ModelPoseCache cache;
    cache.Build(skel, All(untouched));
    settings.weight = 0.0f;
    CHECK(SolveTwoBone(skel, All(untouched), cache, kLeg, settings).valid);
    CHECK(std::memcmp(untouched.Data(), original.Data(), sizeof(BoneTransform) * original.Size()) == 0);

    Array<BoneTransform> full = BentLeg(skel);
    ModelPoseCache fullCache;
    fullCache.Build(skel, All(full));
    settings.weight = 1.0f;
    CHECK(SolveTwoBone(skel, All(full), fullCache, kLeg, settings).reached);

    Array<BoneTransform> half = BentLeg(skel);
    ModelPoseCache halfCache;
    halfCache.Build(skel, All(half));
    settings.weight = 0.5f;
    const IkResult result = SolveTwoBone(skel, All(half), halfCache, kLeg, settings);
    CHECK_FALSE(result.reached);
    for (const i32 bone : {i32{kThigh}, i32{kShin}})
    {
        const Quaternion between = Slerp(original[bone].rotation, full[bone].rotation, 0.5f);
        CHECK(std::abs(Dot(half[bone].rotation, between)) > 1.0f - 1.0e-6f);
    }
    ModelPoseCache originalCache;
    originalCache.Build(skel, Span<const BoneTransform>{original.Data(), original.Size()});
    CHECK(result.error > 1.0e-3f);
    CHECK(result.error < Length(At(originalCache, kFoot) - settings.target));

    // matchRotation: the end bone takes the target's model rotation.
    Array<BoneTransform> turned = BentLeg(skel);
    ModelPoseCache turnedCache;
    turnedCache.Build(skel, All(turned));
    settings.weight = 1.0f;
    settings.matchRotation = true;
    settings.targetRotation = Quaternion::FromAxisAngle(Normalized(Float3{1, 1, 0}), 0.7f);
    CHECK(SolveTwoBone(skel, All(turned), turnedCache, kLeg, settings).reached);
    CHECK(std::abs(Dot(ik::RotationOf(turnedCache.At(kFoot)), settings.targetRotation)) > 1.0f - 1.0e-5f);
}

TEST_CASE("ik: a solve rebuilds from the chain down, bones above and beside never move")
{
    Skeleton skel{kBoneCount};
    BuildBody(skel);
    Array<BoneTransform> pose = BentLeg(skel);
    ModelPoseCache cache;
    cache.Build(skel, All(pose));
    Float4x4 above[kBoneCount];
    for (i32 i = 0; i < kBoneCount; ++i)
    {
        above[i] = cache.At(i);
    }
    TwoBoneIkSettings settings;
    settings.target = Float3{1.5f, 5, 2};
    CHECK(SolveTwoBone(skel, All(pose), cache, kLeg, settings).reached);
    for (const i32 bone : {i32{kPelvis}, i32{kSpine}, i32{kNeck}, i32{kHead}})
    {
        CHECK(std::memcmp(&above[bone], &cache.At(bone), sizeof(Float4x4)) == 0);
    }
    // And what it rebuilt matches a full build of the pose it left.
    ModelPoseCache fresh;
    fresh.Build(skel, All(pose));
    for (i32 i = 0; i < kBoneCount; ++i)
    {
        CHECK(Length(At(cache, i) - At(fresh, i)) < 1.0e-5f);
    }
    CHECK(Length(At(cache, kToe) - At(cache, kFoot)) == doctest::Approx(1.0f)); // carried along
}

TEST_CASE("ik aim: reaches within its limit, clamps beyond it, shares the swing, holds its up axis")
{
    Skeleton skel{kBoneCount};
    BuildBody(skel);
    const i32 spine[] = {kSpine, kNeck, kHead};
    const f32 shares[] = {0.3f, 0.5f, 1.0f};
    const Float3 head{0, 13.5f, 0};

    SUBCASE("within the limit: exact, every bone turned, no roll")
    {
        Array<BoneTransform> pose = BindPose(skel);
        ModelPoseCache cache;
        cache.Build(skel, All(pose));
        AimIkSettings settings;
        settings.target = head + Float3{3, 1, 5};
        const IkResult result = SolveAim(skel, All(pose), cache, spine, shares, settings);
        CHECK(result.valid);
        CHECK(result.reached);
        CHECK(result.error < 1.0e-3f);
        const Float3 aim = Axis(cache, kHead, 2);
        CHECK(Dot(aim, Normalized(settings.target - At(cache, kHead))) > 1.0f - 1.0e-6f);
        for (const i32 bone : spine)
        {
            CHECK(std::abs(pose[bone].rotation.w) < 1.0f - 1.0e-4f); // each took a share
        }
        // The up axis leans where the animated up did (straight up), turned only by the swing.
        const Float3 up = Axis(cache, kHead, 1);
        const Float3 upWanted = Normalized(Float3{0, 1, 0} - aim * Dot(Float3{0, 1, 0}, aim));
        CHECK(Dot(up, upWanted) > 1.0f - 1.0e-5f);
    }
    SUBCASE("beyond the limit: stopped at maxAngle from the animated direction")
    {
        Array<BoneTransform> pose = BindPose(skel);
        ModelPoseCache cache;
        cache.Build(skel, All(pose));
        AimIkSettings settings;
        settings.target = head + Float3{5, 0, -1};
        const IkResult result = SolveAim(skel, All(pose), cache, spine, shares, settings);
        CHECK(result.valid);
        CHECK_FALSE(result.reached);
        CHECK(std::acos(Clamp(Dot(Axis(cache, kHead, 2), Float3{0, 0, 1}), -1.0f, 1.0f)) ==
              doctest::Approx(60.0f * kDegToRad).epsilon(1e-3));
        CHECK(Finite(All(pose)));
    }
    SUBCASE("an up direction: the head rolls toward it")
    {
        Array<BoneTransform> pose = BindPose(skel);
        ModelPoseCache cache;
        cache.Build(skel, All(pose));
        AimIkSettings settings;
        settings.target = head + Float3{0, 0, 10};
        settings.hasUp = true;
        settings.up = Float3{1, 0, 0};
        CHECK(SolveAim(skel, All(pose), cache, Span<const i32>{spine + 2, 1}, {}, settings).reached);
        CHECK(Dot(Axis(cache, kHead, 1), Float3{1, 0, 0}) > 1.0f - 1.0e-5f);
    }
    SUBCASE("weight 0: byte for byte")
    {
        Array<BoneTransform> pose = BindPose(skel);
        const Array<BoneTransform> original = BindPose(skel);
        ModelPoseCache cache;
        cache.Build(skel, All(pose));
        AimIkSettings settings;
        settings.target = head + Float3{3, 1, 5};
        settings.weight = 0.0f;
        CHECK(SolveAim(skel, All(pose), cache, spine, shares, settings).valid);
        CHECK(std::memcmp(pose.Data(), original.Data(), sizeof(BoneTransform) * pose.Size()) == 0);
    }
}

TEST_CASE("ik: no allocation once the cache is sized")
{
    Skeleton skel{kBoneCount};
    BuildBody(skel);
    TrackingAllocator counting{DefaultAllocator()};
    ModelPoseCache cache{counting};
    Array<BoneTransform> pose = BentLeg(skel);
    const i32 spine[] = {kSpine, kNeck, kHead};
    const f32 shares[] = {0.3f, 0.5f, 1.0f};
    TwoBoneIkSettings leg;
    AimIkSettings look;
    leg.target = Float3{1, 5, 1};
    look.target = Float3{2, 14, 6};
    cache.Build(skel, All(pose));
    (void)SolveTwoBone(skel, All(pose), cache, kLeg, leg);
    (void)SolveAim(skel, All(pose), cache, spine, shares, look);
    const u64 sized = counting.TotalAllocations();
    for (i32 frame = 0; frame < 20; ++frame)
    {
        cache.Build(skel, All(pose));
        leg.target = Float3{1.0f + 0.1f * static_cast<f32>(frame), 5, 1};
        look.target = Float3{2, 14, 6.0f - 0.2f * static_cast<f32>(frame)};
        (void)SolveTwoBone(skel, All(pose), cache, kLeg, leg);
        (void)SolveAim(skel, All(pose), cache, spine, shares, look);
    }
    CHECK(counting.TotalAllocations() == sized);
}
