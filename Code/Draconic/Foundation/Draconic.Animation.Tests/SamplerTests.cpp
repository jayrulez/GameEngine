// AnimationSampler: track/clip sampling + pose blending. Covers the ported math directly (no
// Sedulous sampler test existed).
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"

import draconic.foundation;
import draconic.animation;

using namespace draconic::foundation;
using namespace draconic::animation;

TEST_CASE("sampler: Float3 track linear interpolation")
{
    AnimationTrack<Float3> track;
    track.AddKeyframe(0.0f, Float3{0, 0, 0});
    track.AddKeyframe(1.0f, Float3{10, 0, 0});
    CHECK(NearlyEqual(SampleVec3(&track, 0.0f), Float3{0, 0, 0}));
    CHECK(NearlyEqual(SampleVec3(&track, 0.5f), Float3{5, 0, 0}));
    CHECK(NearlyEqual(SampleVec3(&track, 1.0f), Float3{10, 0, 0}));
    CHECK(NearlyEqual(SampleVec3(&track, 2.0f), Float3{10, 0, 0})); // clamps past last
    // empty / null returns the default
    CHECK(NearlyEqual(SampleVec3(nullptr, 0.5f, Float3{9, 9, 9}), Float3{9, 9, 9}));
}

TEST_CASE("sampler: Step interpolation holds previous value")
{
    AnimationTrack<Float3> track;
    track.interpolation = InterpolationMode::Step;
    track.AddKeyframe(0.0f, Float3{0, 0, 0});
    track.AddKeyframe(1.0f, Float3{10, 0, 0});
    CHECK(NearlyEqual(SampleVec3(&track, 0.5f), Float3{0, 0, 0})); // holds prev until next key
}

TEST_CASE("sampler: SampleClip animates targeted bones, bind pose otherwise")
{
    Skeleton skel{2};
    skel.Bones()[0].localBindPose.position = Float3{0, 0, 0};
    skel.Bones()[1].localBindPose.position = Float3{7, 7, 7};

    AnimationClip clip{u8"move", 1.0f};
    AnimationClip::Vec3Track* pos = clip.GetOrCreatePositionTrack(0);
    pos->AddKeyframe(0.0f, Float3{0, 0, 0});
    pos->AddKeyframe(1.0f, Float3{0, 10, 0});

    BoneTransform poses[2] = {};
    SampleClip(clip, skel, 0.5f, Span<BoneTransform>{poses, 2});
    CHECK(NearlyEqual(poses[0].position, Float3{0, 5, 0})); // animated
    CHECK(NearlyEqual(poses[1].position, Float3{7, 7, 7})); // untouched -> bind pose
}

TEST_CASE("sampler: BlendPoses + AdditivePoses")
{
    BoneTransform a[1] = {};
    a[0].position = Float3{0, 0, 0};
    a[0].scale = Float3{1, 1, 1};
    BoneTransform b[1] = {};
    b[0].position = Float3{10, 0, 0};
    b[0].scale = Float3{3, 3, 3};
    BoneTransform out[1] = {};

    BlendPoses(Span<const BoneTransform>{a, 1}, Span<const BoneTransform>{b, 1}, 0.5f,
               Span<BoneTransform>{out, 1});
    CHECK(NearlyEqual(out[0].position, Float3{5, 0, 0}));
    CHECK(NearlyEqual(out[0].scale, Float3{2, 2, 2}));

    BoneTransform base[1] = {};
    base[0].position = Float3{1, 0, 0};
    base[0].scale = Float3{1, 1, 1};
    BoneTransform add[1] = {};
    add[0].position = Float3{0, 5, 0};
    add[0].scale = Float3{1, 1, 1};
    AdditivePoses(Span<const BoneTransform>{base, 1}, Span<const BoneTransform>{add, 1}, 1.0f,
                  Span<BoneTransform>{out, 1});
    CHECK(NearlyEqual(out[0].position, Float3{1, 5, 0})); // base + additive*weight
}
