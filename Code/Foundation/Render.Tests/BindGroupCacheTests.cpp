// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The passes' bind group cache keeps a group only while every view it binds is the same view of
// the same texture. TAA once checked only its jittered colour: on the player's first frame the
// motion texture differed from every later frame's while the colour did not, so the group built
// for one of its two history textures kept frame 0's motion vectors, and every other frame's
// edges flickered (2026-10-02). These replay that sequence on stand-in pointers; no device.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.rhi;
import foundation.render;

using namespace foundation::core;
using namespace foundation::render;
namespace rhi = foundation::rhi;

namespace
{
    // Distinct stand-in addresses: the cache only compares and returns them.
    alignas(8) u8 g_slots[16][8];
    [[nodiscard]] rhi::TextureView* View(int i) { return reinterpret_cast<rhi::TextureView*>(g_slots[i]); }
    [[nodiscard]] rhi::BindGroup* Group(int i) { return reinterpret_cast<rhi::BindGroup*>(g_slots[8 + i]); }

    // TAA's inputs: jittered colour, motion, depth.
    [[nodiscard]] BindGroupInputs<3> Inputs(int colour, u64 colourGen, int motion, u64 motionGen, int depth,
                                            u64 depthGen)
    {
        BindGroupInputs<3> in;
        in.Set(0, View(colour), colourGen);
        in.Set(1, View(motion), motionGen);
        in.Set(2, View(depth), depthGen);
        return in;
    }
}

TEST_CASE("bind group cache: a group is rebuilt when any input changed, not only the first")
{
    BindGroupCache<3> cache;
    rhi::TextureView* historyA = View(6);
    rhi::TextureView* historyB = View(7);
    rhi::BindGroup* stale = nullptr;

    // Frame 0 reads history A: colour 0, motion 1 (the first frame's), depth 3.
    const BindGroupInputs<3> frame0 = Inputs(0, 9, 1, 3, 3, 1);
    CHECK(cache.Find(historyA, frame0, stale) == nullptr);
    CHECK(stale == nullptr);
    cache.Store(historyA, frame0, Group(0));
    // Frame 1 reads history B with this frame's motion texture (2): a group of its own.
    const BindGroupInputs<3> steady = Inputs(0, 9, 2, 2, 3, 1);
    CHECK(cache.Find(historyB, steady, stale) == nullptr);
    cache.Store(historyB, steady, Group(1));

    // Frame 2 reads history A again: the same colour and depth, but motion 2. The frame 0 group
    // binds the wrong motion vectors, so it is handed back to destroy and not returned.
    CHECK(cache.Find(historyA, steady, stale) == nullptr);
    CHECK(stale == Group(0));
    CHECK(cache.Size() == 1u);
    cache.Store(historyA, steady, Group(2));

    // Steady state: both history textures hit with this frame's inputs.
    CHECK(cache.Find(historyA, steady, stale) == Group(2));
    CHECK(stale == nullptr);
    CHECK(cache.Find(historyB, steady, stale) == Group(1));

    // The same view back over another texture (a new generation) is a different input.
    CHECK(cache.Find(historyB, Inputs(0, 9, 2, 2, 3, 2), stale) == nullptr);
    CHECK(stale == Group(1));
    // History A is untouched by B's miss.
    CHECK(cache.Find(historyA, steady, stale) == Group(2));

    // An input missing: no group can be built.
    BindGroupInputs<3> missing = steady;
    missing.Set(1, nullptr, 0);
    CHECK_FALSE(missing.Complete());
    CHECK(steady.Complete());

    // Teardown hands every held group to the owner.
    usize released = 0;
    cache.Release([&](rhi::BindGroup*) { ++released; });
    CHECK(released == 1u); // A's (B's was handed back as stale above)
    CHECK(cache.Size() == 0u);
}

// SSGI's composite also binds a scene's sky lighting buffer: two views of different scenes share
// one history texture slot when they take turns, so the buffer (and its context's generation) is an
// input like the views, and a group built with another scene's sky is not handed back.
TEST_CASE("bind group cache: the bound buffer and its generation are inputs too")
{
    BindGroupCache<3> cache;
    rhi::TextureView* history = View(6);
    rhi::BindGroup* stale = nullptr;
    rhi::Buffer* skyA = reinterpret_cast<rhi::Buffer*>(g_slots[4]);
    rhi::Buffer* skyB = reinterpret_cast<rhi::Buffer*>(g_slots[5]);

    BindGroupInputs<3> withA = Inputs(0, 1, 1, 1, 3, 1);
    withA.SetBuffer(skyA, 7);
    cache.Store(history, withA, Group(0));
    CHECK(cache.Find(history, withA, stale) == Group(0));

    BindGroupInputs<3> withB = withA;
    withB.SetBuffer(skyB, 8);
    CHECK(cache.Find(history, withB, stale) == nullptr); // another scene's sky
    CHECK(stale == Group(0));

    cache.Store(history, withB, Group(1));
    BindGroupInputs<3> regenerated = withB;
    regenerated.SetBuffer(skyB, 9); // the same buffer, rebuilt (a new context generation)
    CHECK(cache.Find(history, regenerated, stale) == nullptr);
    CHECK(stale == Group(1));
}
