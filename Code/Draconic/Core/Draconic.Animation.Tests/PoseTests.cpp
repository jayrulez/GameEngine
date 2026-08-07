// AnimationPose view. Ported from Sedulous.Animation.Tests.AnimationPoseTests.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"

import draconic.foundation;
import draconic.animation;

using namespace draconic::foundation;
using namespace draconic::animation;

TEST_CASE("pose: constructor with bone transforms sets bone count")
{
    BoneTransform transforms[4] = {};
    const AnimationPose pose{Span<BoneTransform>{transforms, 4}};
    CHECK(pose.BoneCount() == 4);
    CHECK(pose.HasMorphWeights() == false);
}

TEST_CASE("pose: constructor with morph weights")
{
    BoneTransform transforms[2] = {};
    f32 morphs[3] = {0.5f, 0.0f, 1.0f};
    const AnimationPose pose{Span<BoneTransform>{transforms, 2}, Span<f32>{morphs, 3}};
    CHECK(pose.BoneCount() == 2);
    CHECK(pose.HasMorphWeights() == true);
    CHECK(pose.morphWeights.Size() == 3);
}

TEST_CASE("pose: empty pose has zero bones")
{
    const AnimationPose pose{Span<BoneTransform>{}};
    CHECK(pose.BoneCount() == 0);
    CHECK(pose.HasMorphWeights() == false);
}

TEST_CASE("pose: bone transforms are accessible")
{
    BoneTransform transforms[2] = {};
    transforms[0].position = Float3{1, 2, 3};
    transforms[1].position = Float3{4, 5, 6};
    const AnimationPose pose{Span<BoneTransform>{transforms, 2}};
    CHECK(pose.boneTransforms[0].position.x == 1.0f);
    CHECK(pose.boneTransforms[1].position.x == 4.0f);
}
