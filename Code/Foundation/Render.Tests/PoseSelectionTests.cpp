// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Skinned-crowd pose selection: the renderer's per-instance pose pick (SelectPose) and the crowd-
// authoring helpers that build an Explicit per-instance index array from a grid position (ColumnPose /
// WavePose / ClusterPose). Pure integer math, no GPU: these settle whether each pose mode does what
// it says.
#include <doctest/doctest.h>
#include <initializer_list>
#include "Core/Prelude.h"

import foundation.core;
import foundation.render;

using namespace foundation::core;
using namespace foundation::render;

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
        0; // how often Hash(i)%M coincides with i%M (a linear map would match every time)
    for (u32 i = 0; i < 4096; ++i)
    {
        const u32 p = SelectPose(PoseAssignment::Hashed, i, M, nullptr);
        CHECK(p < M); // in range
        CHECK(p == SelectPose(PoseAssignment::Hashed, i, M,
                              nullptr)); // deterministic (stable per i, so safe for motion blur)
        CHECK(p == static_cast<u32>(HashInteger(i) % M)); // it is the splitmix finalizer
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

TEST_CASE("SelectPose: a clip per instance picks its block; the policy the phase inside it")
{
    // Three clips at 4 poses each: clip c's poses are 4c .. 4c + 3.
    constexpr u32 kPerClip = 4;
    constexpr u32 kPool = 3 * kPerClip;
    for (u32 i = 0; i < 40; ++i)
    {
        for (u32 clip = 0; clip < 3; ++clip)
        {
            const u32 hashed = SelectPose(PoseAssignment::Hashed, i, kPool, nullptr, clip, kPerClip);
            CHECK(hashed / kPerClip == clip); // in its clip's block
            CHECK(hashed % kPerClip == SelectPose(PoseAssignment::Hashed, i, kPerClip, nullptr)); // same phase
            CHECK(SelectPose(PoseAssignment::Sequential, i, kPool, nullptr, clip, kPerClip) ==
                  clip * kPerClip + i % kPerClip);
            // Unison: every instance on a clip at that clip's one phase (a crowd in step).
            CHECK(SelectPose(PoseAssignment::Unison, i, kPool, nullptr, clip, kPerClip) == clip * kPerClip);
        }
    }
    // Explicit indices are phases within the clip.
    const u32 phases[] = {1, 6};
    CHECK(SelectPose(PoseAssignment::Explicit, 0, kPool, phases, 2, kPerClip) == 2 * kPerClip + 1);
    CHECK(SelectPose(PoseAssignment::Explicit, 1, kPool, phases, 1, kPerClip) == 1 * kPerClip + 2); // 6 % 4
    // A clip past the pool wraps; one clip (no posesPerClip) selects as before.
    CHECK(SelectPose(PoseAssignment::Unison, 0, kPool, nullptr, 4, kPerClip) == 1 * kPerClip);
    CHECK(SelectPose(PoseAssignment::Sequential, 7, kPool, nullptr) == 7u);
    CHECK(SelectPose(PoseAssignment::Unison, 9, 8, nullptr) == 0u);
}
