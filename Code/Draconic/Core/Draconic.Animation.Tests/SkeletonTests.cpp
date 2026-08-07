// Skeleton hierarchy: name lookup, root/child/order building, world-pose accumulation, and
// skinning-matrix correctness (identity at bind pose). No Sedulous test existed; this covers the
// ported math directly.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"

import draconic.foundation;
import draconic.animation;

using namespace draconic::foundation;
using namespace draconic::animation;

// A 3-bone chain root(0) -> child(1) -> grandchild(2), each translated +Y from its parent.
static void BuildChain(Skeleton& s)
{
    Array<Bone>& bones = s.Bones();
    bones[0].index = 0;
    bones[0].parentIndex = -1;
    bones[0].name = String{u8"root"};
    bones[0].localBindPose.position = Float3{0, 10, 0};
    bones[1].index = 1;
    bones[1].parentIndex = 0;
    bones[1].name = String{u8"child"};
    bones[1].localBindPose.position = Float3{0, 5, 0};
    bones[2].index = 2;
    bones[2].parentIndex = 1;
    bones[2].name = String{u8"grandchild"};
    bones[2].localBindPose.position = Float3{0, 2, 0};
    s.BuildNameMap();
    s.FindRootBones();
    s.BuildChildIndices();
}

TEST_CASE("skeleton: name map + root finding")
{
    Skeleton s{3};
    BuildChain(s);
    CHECK(s.BoneCount() == 3);
    CHECK(s.FindBone(u8"child") == 1);
    CHECK(s.FindBone(u8"grandchild") == 2);
    CHECK(s.FindBone(u8"missing") == -1);
    CHECK(s.RootBones().Size() == 1);
    CHECK(s.RootBones()[0] == 0);
}

TEST_CASE("skeleton: world poses accumulate down the hierarchy (bind pose)")
{
    Skeleton s{3};
    BuildChain(s);

    Float4x4 world[3];
    s.ComputeWorldPoses(Span<const BoneTransform>{}, Span<Float4x4>{world, 3});

    // Pure translations compose additively along the chain (+Y).
    CHECK(NearlyEqual(world[0].m[3][1], 10.0f));
    CHECK(NearlyEqual(world[1].m[3][1], 15.0f));
    CHECK(NearlyEqual(world[2].m[3][1], 17.0f));
}

TEST_CASE("skeleton: skinning matrices are identity at the bind pose")
{
    Skeleton s{3};
    BuildChain(s);
    s.ComputeInverseBindPoses();

    Float4x4 skin[3];
    s.ComputeSkinningMatrices(Span<const BoneTransform>{}, Span<Float4x4>{skin, 3});

    // skin = inverseBind * world; evaluated at the bind pose this is identity for every bone.
    for (int i = 0; i < 3; ++i)
    {
        CHECK(NearlyEqual(skin[i].m[0][0], 1.0f));
        CHECK(NearlyEqual(skin[i].m[1][1], 1.0f));
        CHECK(NearlyEqual(skin[i].m[2][2], 1.0f));
        CHECK(NearlyEqual(skin[i].m[3][0], 0.0f));
        CHECK(NearlyEqual(skin[i].m[3][1], 0.0f));
        CHECK(NearlyEqual(skin[i].m[3][2], 0.0f));
    }
}

TEST_CASE("skeleton: hierarchical order lists parents before children")
{
    // A child declared BEFORE its parent in the array must still evaluate after it.
    Skeleton s{2};
    Array<Bone>& bones = s.Bones();
    bones[0].index = 0;
    bones[0].parentIndex = 1; // bone 0's parent is bone 1
    bones[1].index = 1;
    bones[1].parentIndex = -1; // bone 1 is the root
    bones[0].localBindPose.position = Float3{0, 1, 0};
    bones[1].localBindPose.position = Float3{0, 100, 0};
    s.FindRootBones();
    s.BuildChildIndices();

    Float4x4 world[2];
    s.ComputeWorldPoses(Span<const BoneTransform>{}, Span<Float4x4>{world, 2});
    // If order were wrong, bone 0 would miss bone 1's transform. Expect 100 + 1 = 101.
    CHECK(NearlyEqual(world[0].m[3][1], 101.0f));
    CHECK(NearlyEqual(world[1].m[3][1], 100.0f));
}
