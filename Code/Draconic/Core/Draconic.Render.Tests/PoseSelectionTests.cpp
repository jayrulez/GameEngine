// Skinned-crowd pose selection: the renderer's per-instance pose pick (SelectPose) and the crowd-
// authoring helpers that build an Explicit per-instance index array from a grid position (ColumnPose /
// WavePose / ClusterPose). Pure integer math, no GPU - this is where the "does the pose mode do what it
// says" question is settled (the visual muddle in the sample is clip bucketing, not this math).
#include <doctest/doctest.h>
#include <initializer_list>
#include "Draconic.Foundation/Prelude.h"

import draconic.foundation;
import draconic.render;

using namespace draconic::foundation;
using namespace draconic::render;

TEST_CASE("SelectPose: Sequential is i % M")
{
    for (u32 M : {1u, 7u, 32u})
    {
        for (u32 i = 0; i < 100; ++i)
        {
            CHECK(SelectPose(PoseAssignment::Sequential, i, M, nullptr) == i % M);
        }
    }
}

TEST_CASE("SelectPose: poseCount == 0 is a safe 0 (no palettes)")
{
    CHECK(SelectPose(PoseAssignment::Sequential, 5, 0, nullptr) == 0u);
    CHECK(SelectPose(PoseAssignment::Hashed, 5, 0, nullptr) == 0u);
    const u32 idx[] = {9};
    CHECK(SelectPose(PoseAssignment::Explicit, 0, 0, idx) == 0u);
}

TEST_CASE("SelectPose: Hashed is deterministic, in range, and NOT the identity/linear map")
{
    constexpr u32 M = 32;
    bool bucketHit[M] = {};
    u32 matchesModulo =
        0; // how often Hash(i)%M coincides with i%M (a linear map would be ALL of them)
    for (u32 i = 0; i < 4096; ++i)
    {
        const u32 p = SelectPose(PoseAssignment::Hashed, i, M, nullptr);
        CHECK(p < M); // in range
        CHECK(p == SelectPose(PoseAssignment::Hashed, i, M,
                              nullptr)); // deterministic (stable per i -> motion blur safe)
        CHECK(p == static_cast<u32>(HashInteger(i) % M)); // it IS the splitmix finalizer
        bucketHit[p] = true;
        if (p == i % M)
        {
            ++matchesModulo;
        }
    }
    for (u32 b = 0; b < M; ++b)
    {
        CHECK(bucketHit[b]);
    } // every pose bucket used (good spread)
    CHECK(matchesModulo < 512); // decorrelated from i%M: nowhere near all 4096
}

TEST_CASE("SelectPose: Explicit uses the caller array, clamped mod M")
{
    constexpr u32 M = 8;
    const u32 idx[] = {0, 3, 7, 8, 15, 100}; // includes values >= M to prove the mod-clamp
    CHECK(SelectPose(PoseAssignment::Explicit, 0, M, idx) == 0u);
    CHECK(SelectPose(PoseAssignment::Explicit, 1, M, idx) == 3u);
    CHECK(SelectPose(PoseAssignment::Explicit, 2, M, idx) == 7u);
    CHECK(SelectPose(PoseAssignment::Explicit, 3, M, idx) == 0u); // 8 % 8
    CHECK(SelectPose(PoseAssignment::Explicit, 4, M, idx) == 7u); // 15 % 8
    CHECK(SelectPose(PoseAssignment::Explicit, 5, M, idx) == 4u); // 100 % 8
}

TEST_CASE("SelectPose: Explicit with a null array degrades to Hashed (the extract fallback)")
{
    constexpr u32 M = 16;
    for (u32 i = 0; i < 64; ++i)
    {
        CHECK(SelectPose(PoseAssignment::Explicit, i, M, nullptr) ==
              SelectPose(PoseAssignment::Hashed, i, M, nullptr));
    }
}

TEST_CASE("ColumnPose: constant down a column, steps across columns")
{
    constexpr u32 M = 32;
    // Same column -> same pose regardless of row (ColumnPose ignores row by construction).
    for (u32 col = 0; col < 50; ++col)
    {
        const u32 p = ColumnPose(col, M);
        CHECK(p == col % M);
        for (u32 row = 0; row < 20; ++row)
        {
            CHECK(ColumnPose(col, M) == p);
        }
    }
    // Adjacent columns differ while col < M (no aliasing until the modulo wraps).
    for (u32 col = 0; col + 1 < M; ++col)
    {
        CHECK(ColumnPose(col, M) != ColumnPose(col + 1, M));
    }
}

TEST_CASE("WavePose: diagonal gradient - equal along anti-diagonals, steps along the gradient")
{
    constexpr u32 M = 32;
    for (u32 col = 0; col < 40; ++col)
    {
        for (u32 row = 0; row < 40; ++row)
        {
            CHECK(WavePose(col, row, M) == (col + row) % M);
            // Anti-diagonal invariance: moving +1 col / -0 row equals +0 col / +1 row (the wave is diagonal).
            CHECK(WavePose(col + 1, row, M) == WavePose(col, row + 1, M));
            // A step along the gradient advances the phase by exactly one bucket (mod M).
            CHECK(WavePose(col + 1, row, M) == (WavePose(col, row, M) + 1) % M);
        }
    }
}

TEST_CASE("ClusterPose: constant within a cell, and neighbouring cells differ")
{
    constexpr u32 M = 32;
    constexpr u32 cell = 4;
    // Every character inside a 4x4 cell shares one pose.
    for (u32 cx = 0; cx < 8; ++cx)
    {
        for (u32 cz = 0; cz < 8; ++cz)
        {
            const u32 p = ClusterPose(cx * cell, cz * cell, cell, M);
            CHECK(p < M);
            for (u32 dx = 0; dx < cell; ++dx)
            {
                for (u32 dz = 0; dz < cell; ++dz)
                {
                    CHECK(ClusterPose(cx * cell + dx, cz * cell + dz, cell, M) == p);
                }
            }
        }
    }
    // The cell->phase map is a hash, not a ramp: over a field of cells, adjacent cells usually differ.
    // (A linear map like ColumnPose would make whole rows/cols of cells identical.) Count disagreements.
    u32 differ = 0, pairs = 0;
    for (u32 cx = 0; cx < 16; ++cx)
    {
        for (u32 cz = 0; cz < 16; ++cz)
        {
            const u32 here = ClusterPose(cx * cell, cz * cell, cell, M);
            const u32 right = ClusterPose((cx + 1) * cell, cz * cell, cell, M);
            if (here != right)
            {
                ++differ;
            }
            ++pairs;
        }
    }
    CHECK(differ > pairs * 3 / 4); // the vast majority of neighbouring cells differ (hash spread)
}
