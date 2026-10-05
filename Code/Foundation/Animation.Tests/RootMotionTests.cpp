// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Root motion deltas (root-motion.md P1, the spec's tests): a looping clip's deltas over many
// frames sum to its travel per loop, with nothing lost at the wrap and a step of two loops counting
// both; a clip that does not loop stops at its end; a 50/50 blend of two walks moves at the mean
// speed; a crossfade changes speed without a step at either end; a turn clip's yaw accumulates and
// the travel after it goes the new way; only the base layer moves the character.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

#include <cmath>
#include <initializer_list>

import foundation.core;
import foundation.animation;

using namespace foundation::core;
using namespace foundation::animation;

namespace
{
    // A clip whose root walks `metres` along +Z over `seconds` (and turns `turn` radians), as the
    // cook bakes it.
    void Walker(AnimationClip& clip, f32 metres, f32 seconds = 1.0f, f32 turn = 0.0f, bool loop = true)
    {
        clip.duration = seconds;
        clip.isLooping = loop;
        clip.rootMotion.horizontal = true;
        clip.rootMotion.yaw = turn != 0.0f;
        for (i32 i = 0; i <= 30; ++i)
        {
            const f32 t = static_cast<f32>(i) / 30.0f;
            clip.rootMotion.times.PushBack(t * seconds);
            clip.rootMotion.positions.PushBack(Float3{0.0f, 0.0f, metres * t});
            clip.rootMotion.yaws.PushBack(turn * t);
        }
        // A track so a graph state has a pose to sample (its root motion is the curve's).
        clip.GetOrCreatePositionTrack(0)->AddKeyframe(0.0f, Float3{0, 1, 0});
        clip.GetOrCreatePositionTrack(0)->AddKeyframe(seconds, Float3{0, 1, 0});
    }

    void OneBone(Skeleton& s)
    {
        s.Bones()[0].index = 0;
        s.Bones()[0].parentIndex = -1;
        s.FindRootBones();
        s.BuildChildIndices();
    }
}

TEST_CASE("root motion deltas: a loop's frames sum to its travel, nothing lost at the wrap")
{
    AnimationClip walk{u8"walk"};
    Walker(walk, 2.0f);
    Skeleton skeleton{1};
    OneBone(skeleton);
    AnimationPlayer player{skeleton};
    player.Play(&walk);
    RootMotionDelta total;
    for (i32 frame = 0; frame < 75; ++frame) // 3 s at 25 Hz: three loops, wrapping mid-frame
    {
        player.Update(0.04f);
        total = Compose(total, player.ConsumeRootMotion());
    }
    CHECK(total.translation.z == doctest::Approx(6.0f).epsilon(1e-4));
    CHECK(total.translation.x == doctest::Approx(0.0f));
    CHECK(player.ConsumeRootMotion().IsZero()); // read: reset

    // One step of two and a half loops counts every loop.
    AnimationPlayer far{skeleton};
    far.Play(&walk);
    far.Update(0.3f);
    (void)far.ConsumeRootMotion();
    far.Update(2.5f);
    CHECK(far.ConsumeRootMotion().translation.z == doctest::Approx(5.0f).epsilon(1e-4));

    // Played backwards it walks back.
    AnimationPlayer back{skeleton};
    back.Play(&walk);
    back.speed = -1.0f;
    back.Update(1.5f);
    CHECK(back.ConsumeRootMotion().translation.z == doctest::Approx(-3.0f).epsilon(1e-4));

    // A clip that does not loop stops at its end.
    AnimationClip once{u8"once"};
    Walker(once, 2.0f, 1.0f, 0.0f, false);
    AnimationPlayer stop{skeleton};
    stop.Play(&once);
    stop.Update(0.75f);
    stop.Update(0.75f);
    CHECK(stop.ConsumeRootMotion().translation.z == doctest::Approx(2.0f).epsilon(1e-4));
}

TEST_CASE("root motion deltas: a turn accumulates, and the travel after it goes the new way")
{
    // A quarter turn while walking 2 m, per loop: four loops come round, the walk drawing a
    // closed path back near its start rather than a straight 8 m line.
    AnimationClip arc{u8"arc"};
    Walker(arc, 2.0f, 1.0f, kHalfPi);
    Skeleton skeleton{1};
    OneBone(skeleton);
    AnimationPlayer player{skeleton};
    player.Play(&arc);
    RootMotionDelta total;
    for (i32 frame = 0; frame < 120; ++frame)
    {
        player.Update(1.0f / 30.0f);
        total = Compose(total, player.ConsumeRootMotion());
    }
    CHECK(total.yaw == doctest::Approx(4.0f * kHalfPi).epsilon(1e-3));
    CHECK(Length(total.translation) < 0.1f); // round the square, back where it began
    // One loop on its own: a quarter turn, and its travel in the frame it started facing.
    const RootMotionDelta one = ClipRootMotion(arc, 0.0f, 1.0f, true);
    CHECK(one.yaw == doctest::Approx(kHalfPi).epsilon(1e-4));
    CHECK(one.translation.z == doctest::Approx(2.0f).epsilon(1e-3));
}

namespace
{
    // Walk (2 m/s) and Run (4 m/s) in a 1D tree on Speed (0..1), and the same clips as two states.
    struct Gaits
    {
        AnimationClip walk{u8"walk"};
        AnimationClip run{u8"run"};
        AnimationGraph graph;
        i32 speed = -1;
        i32 running = -1;

        explicit Gaits(bool tree)
        {
            Walker(walk, 2.0f);
            Walker(run, 4.0f);
            speed = graph.AddParameter(u8"Speed", AnimationParameterType::Float);
            running = graph.AddParameter(u8"Running", AnimationParameterType::Bool);
            auto layer = MakeUnique<AnimationLayer>(DefaultAllocator(), StringView{u8"Base"});
            if (tree)
            {
                auto blend = MakeUnique<BlendTree1D>(DefaultAllocator());
                blend->parameterIndex = speed;
                blend->AddEntry(0.0f, &walk);
                blend->AddEntry(1.0f, &run);
                UniquePtr<IAnimationStateNode> node = Move(blend);
                layer->AddState(MakeUnique<AnimationGraphState>(DefaultAllocator(), StringView{u8"Move"},
                                                                Move(node)));
            }
            else
            {
                layer->AddState(MakeUnique<AnimationGraphState>(DefaultAllocator(), StringView{u8"Walk"},
                                                                MakeUnique<ClipStateNode>(DefaultAllocator(), &walk)));
                layer->AddState(MakeUnique<AnimationGraphState>(DefaultAllocator(), StringView{u8"Run"},
                                                                MakeUnique<ClipStateNode>(DefaultAllocator(), &run)));
                auto toRun = MakeUnique<AnimationGraphTransition>(DefaultAllocator());
                toRun->sourceStateIndex = 0;
                toRun->destStateIndex = 1;
                toRun->duration = 0.5f;
                toRun->AddBoolCondition(running, true);
                layer->AddTransition(static_cast<UniquePtr<AnimationGraphTransition>&&>(toRun));
            }
            graph.AddLayer(static_cast<UniquePtr<AnimationLayer>&&>(layer));
        }
    };
}

TEST_CASE("root motion deltas: a 50/50 blend of two walks moves at the mean speed")
{
    Gaits gaits{true};
    Skeleton skeleton{1};
    OneBone(skeleton);
    AnimationGraphPlayer player{gaits.graph, skeleton};
    player.SetFloat(gaits.speed, 0.5f);
    RootMotionDelta total;
    for (i32 frame = 0; frame < 60; ++frame) // 2 s
    {
        player.Update(1.0f / 30.0f);
        total = Compose(total, player.ConsumeRootMotion());
    }
    CHECK(total.translation.z == doctest::Approx(6.0f).epsilon(1e-3)); // 3 m/s
    player.SetFloat(gaits.speed, 1.0f);
    player.Update(0.5f);
    CHECK(player.ConsumeRootMotion().translation.z == doctest::Approx(2.0f).epsilon(1e-3)); // all run
}

TEST_CASE("root motion deltas: a crossfade changes speed without a step at either end")
{
    Gaits gaits{false};
    Skeleton skeleton{1};
    OneBone(skeleton);
    AnimationGraphPlayer player{gaits.graph, skeleton};
    const f32 dt = 1.0f / 60.0f;
    f32 previous = -1.0f;
    f32 largestJump = 0.0f;
    for (i32 frame = 0; frame < 120; ++frame)
    {
        if (frame == 30)
        {
            player.SetBool(gaits.running, true); // a half-second fade from 2 to 4 m/s
        }
        player.Update(dt);
        const f32 step = player.ConsumeRootMotion().translation.z;
        if (previous >= 0.0f)
        {
            largestJump = Max(largestJump, Abs(step - previous));
        }
        previous = step;
    }
    CHECK(previous == doctest::Approx(4.0f * dt).epsilon(1e-3)); // running, after the fade
    // The fade spreads the change over 30 frames: no frame differs from the one before by more
    // than a few times the even share (2 m/s * dt / 30 frames).
    CHECK(largestJump < 3.0f * (2.0f * dt / 30.0f));
}

TEST_CASE("root motion deltas: only the base layer moves the character")
{
    AnimationClip still{u8"still"};
    Walker(still, 0.0f);
    AnimationClip walk{u8"walk"};
    Walker(walk, 2.0f);
    AnimationGraph graph;
    auto base = MakeUnique<AnimationLayer>(DefaultAllocator(), StringView{u8"Base"});
    base->AddState(MakeUnique<AnimationGraphState>(DefaultAllocator(), StringView{u8"Stand"},
                                                   MakeUnique<ClipStateNode>(DefaultAllocator(), &still)));
    graph.AddLayer(static_cast<UniquePtr<AnimationLayer>&&>(base));
    auto upper = MakeUnique<AnimationLayer>(DefaultAllocator(), StringView{u8"Upper"});
    upper->AddState(MakeUnique<AnimationGraphState>(DefaultAllocator(), StringView{u8"Walk"},
                                                    MakeUnique<ClipStateNode>(DefaultAllocator(), &walk)));
    graph.AddLayer(static_cast<UniquePtr<AnimationLayer>&&>(upper));
    Skeleton skeleton{1};
    OneBone(skeleton);
    AnimationGraphPlayer player{graph, skeleton};
    player.Update(0.5f);
    CHECK(player.ConsumeRootMotion().IsZero());
}
