// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// engine.animation property animator: scene integration. A clip drives a reflected property on the
// owning entity's component over a simulated tick sequence; loop wrapping; failed-track disable.

#include <doctest/doctest.h>
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

import foundation.core;
import foundation.scene;
import foundation.propertyanimation;
import foundation.propertyanimation.resource;
import engine.animation;

using namespace foundation::core;
namespace scene = foundation::scene;
using namespace foundation::propertyanimation;
using namespace engine::animation;

// Generated below by REFLECT_VALUE(AnimTarget); forward-declared so EnsureReflected can invoke it.
void RttiRegisterValue_AnimTarget();

namespace
{
    // A target component the clip animates: a Float3 position and a scalar. Reflected so the binding
    // resolver can walk "position" / "value"; its manager is added to the scene under this type name.
    struct AnimTarget
    {
        Float3 position{0.0f, 0.0f, 0.0f};
        f32 value = 0.0f;
    };

    inline void Serialize(ISerializer& ar, AnimTarget& c)
    {
        foundation::core::Serialize(ar, "position", c.position);
        foundation::core::Serialize(ar, "value", c.value);
    }

    class AnimTargetManager final : public scene::SerializableComponentManager<AnimTarget>
    {
    public:
        AnimTargetManager() : scene::SerializableComponentManager<AnimTarget>(u8"anim_target") {}
    };

    // Build a runtime clip driving AnimTarget.position over 1s (0 to (10,0,0)), loop.
    RefPtr<PropertyAnimationClipResource> MakePositionClip()
    {
        RefPtr<PropertyAnimationClipResource> res =
            MakeRef<PropertyAnimationClipResource>(DefaultAllocator());
        PropertyTrack t;
        t.componentType = String(u8"AnimTarget");
        t.propertyPath = String(u8"position");
        t.kind = TrackValueKind::Float3;
        CurveKey a;
        a.time = 0.0f;
        a.value = 0.0f;
        CurveKey b;
        b.time = 1.0f;
        b.value = 10.0f;
        t.channels[0].AddKey(a);
        t.channels[0].AddKey(b);
        res->clip.tracks.PushBack(Move(t));
        res->clip.duration = res->clip.ComputeDuration();
        return res;
    }

    void EnsureReflected()
    {
        static bool done = false;
        if (done)
        {
            return;
        }
        done = true;
        RegisterCoreTypes();
        RegisterAnimationComponentReflection(); // PropertyAnimatorComponent and the others
        RttiRegisterValue_AnimTarget(); // apply AnimTarget's REFLECT_VALUE body (sets the type name)
        GlobalTypeRegistry().Register(TypeOf<AnimTarget>());
    }
}

REFLECT_VALUE(AnimTarget, "rtti::engine::animation::test")
{
    builder.Property<&AnimTarget::position>("position").Property<&AnimTarget::value>("value");
}

TEST_CASE("property animator: the script surface is REFLECTED (methods on the type table)")
{
    EnsureReflected(); // RegisterAnimationComponentReflection built the method table

    // The script surface (animator.of(entity).play()/stop()/...). Asserting the reflected methods,
    // rather than calling C++ directly, is what fails if a `.Method<>` line is dropped or the type
    // is never registered.
    const TypeInfo& type = TypeOf<PropertyAnimatorComponent>();
    const char* expected[] = {"of",     "play", "stop",      "pause",
                              "resume", "time", "isPlaying", "setTime"};
    for (const char* method : expected)
    {
        CHECK_MESSAGE(FindMethod(type, method) != nullptr, "missing reflected method: ", method);
    }
}

TEST_CASE("property animator: a kind/type mismatch disables the track (not a silent per-frame fail)")
{
    EnsureReflected();

    scene::Scene sceneObj{DefaultAllocator()};
    sceneObj.AddSystem<AnimTargetManager>();
    sceneObj.AddSystem<PropertyAnimatorComponentManager>();
    auto* targets = sceneObj.GetSystem<AnimTargetManager>();
    auto* animators = sceneObj.GetSystem<PropertyAnimatorComponentManager>();
    const scene::EntityHandle e = sceneObj.CreateEntity(u8"Mover");
    AnimTarget& target = targets->Add(e);
    target.value = 42.0f; // AnimTarget::value is an f32

    // A Float3 track pointing at the scalar "value" (f32): the kind mismatches the leaf type.
    RefPtr<PropertyAnimationClipResource> res =
        MakeRef<PropertyAnimationClipResource>(DefaultAllocator());
    {
        PropertyTrack t;
        t.componentType = String(u8"AnimTarget");
        t.propertyPath = String(u8"value");
        t.kind = TrackValueKind::Float3; // wrong: value is f32
        CurveKey k0;
        k0.time = 0.0f;
        k0.value = 0.0f;
        CurveKey k1;
        k1.time = 1.0f;
        k1.value = 10.0f;
        t.channels[0].AddKey(k0);
        t.channels[0].AddKey(k1);
        res->clip.tracks.PushBack(Move(t));
        res->clip.duration = res->clip.ComputeDuration();
    }
    PropertyAnimatorComponent& anim = animators->Add(e);
    anim.clip = res.Get();
    anim.autoplay = true;

    sceneObj.Update(0.0f); // binds; the mismatch is caught here
    REQUIRE(anim.bindings.Size() == 1);
    CHECK(anim.bindings[0].disabled); // disabled once, not retried every frame

    sceneObj.Update(0.5f);
    CHECK(target.value == doctest::Approx(42.0f)); // never written
}

TEST_CASE("property animator: a Transform track drives the entity's baked scene transform")
{
    EnsureReflected(); // includes RegisterCoreTypes, which reflects core::Transform

    scene::Scene sceneObj{DefaultAllocator()};
    sceneObj.AddSystem<PropertyAnimatorComponentManager>(); // Transform is built in: no target manager

    auto* animators = sceneObj.GetSystem<PropertyAnimatorComponentManager>();
    REQUIRE(animators != nullptr);
    const scene::EntityHandle e = sceneObj.CreateEntity(u8"Mover");

    // A clip animating Transform.position.x from 0 to 10 over 1s.
    RefPtr<PropertyAnimationClipResource> res =
        MakeRef<PropertyAnimationClipResource>(DefaultAllocator());
    {
        PropertyTrack t;
        t.componentType = String(u8"Transform");
        t.propertyPath = String(u8"position");
        t.kind = TrackValueKind::Float3;
        CurveKey a;
        a.time = 0.0f;
        a.value = 0.0f;
        CurveKey b;
        b.time = 1.0f;
        b.value = 10.0f;
        t.channels[0].AddKey(a);
        t.channels[0].AddKey(b);
        res->clip.tracks.PushBack(Move(t));
        res->clip.duration = res->clip.ComputeDuration();
    }
    PropertyAnimatorComponent& anim = animators->Add(e);
    anim.clip = res.Get();
    anim.autoplay = true;
    anim.loopMode = PropertyLoopMode::Loop;

    sceneObj.Update(0.0f);
    CHECK(sceneObj.GetLocalTransform(e).position.x == doctest::Approx(0.0f));
    sceneObj.Update(0.5f);
    CHECK(sceneObj.GetLocalTransform(e).position.x == doctest::Approx(5.0f));
    sceneObj.Update(0.4f);
    CHECK(sceneObj.GetLocalTransform(e).position.x == doctest::Approx(9.0f));
    // The write went through SetLocalTransform, so the world matrix reflects it after the
    // scene's TransformUpdate phase (which Update runs).
    CHECK(sceneObj.GetWorldMatrix(e).m[3][0] == doctest::Approx(9.0f));
}

TEST_CASE("property animator: a Float3 clip drives the owning component's position over ticks")
{
    EnsureReflected();

    scene::Scene sceneObj{DefaultAllocator()};
    sceneObj.AddSystem<AnimTargetManager>();
    sceneObj.AddSystem<PropertyAnimatorComponentManager>();

    auto* targets = sceneObj.GetSystem<AnimTargetManager>();
    auto* animators = sceneObj.GetSystem<PropertyAnimatorComponentManager>();
    REQUIRE(targets != nullptr);
    REQUIRE(animators != nullptr);

    const scene::EntityHandle e = sceneObj.CreateEntity(u8"Mover");
    AnimTarget& target = targets->Add(e);

    RefPtr<PropertyAnimationClipResource> clip = MakePositionClip();
    PropertyAnimatorComponent& anim = animators->Add(e);
    anim.clip = clip.Get(); // direct runtime object (no resource manager in this test)
    anim.autoplay = true;

    // First tick binds and autostarts at t=0 (position ~ 0).
    sceneObj.Update(0.0f);
    CHECK(target.position.x == doctest::Approx(0.0f));

    // Advance to ~0.5s: the midpoint (5,0,0).
    sceneObj.Update(0.5f);
    CHECK(target.position.x == doctest::Approx(5.0f));

    // Advance to ~0.9s: near the end (9,0,0).
    sceneObj.Update(0.4f);
    CHECK(target.position.x == doctest::Approx(9.0f));

    // Past the end (t=1.1) it loops back to ~0.1 (x ~ 1).
    sceneObj.Update(0.2f);
    CHECK(target.position.x == doctest::Approx(1.0f).epsilon(0.05));
}

TEST_CASE("property animator: Once mode stops at the end; Loop keeps going")
{
    EnsureReflected();
    scene::Scene sceneObj{DefaultAllocator()};
    sceneObj.AddSystem<AnimTargetManager>();
    sceneObj.AddSystem<PropertyAnimatorComponentManager>();
    auto* targets = sceneObj.GetSystem<AnimTargetManager>();
    auto* animators = sceneObj.GetSystem<PropertyAnimatorComponentManager>();

    const scene::EntityHandle e = sceneObj.CreateEntity(u8"Once");
    targets->Add(e);
    RefPtr<PropertyAnimationClipResource> clip = MakePositionClip();
    PropertyAnimatorComponent& anim = animators->Add(e);
    anim.clip = clip.Get();
    anim.loopMode = PropertyLoopMode::Once;

    sceneObj.Update(0.0f);
    sceneObj.Update(2.0f); // way past the end
    CHECK(targets->Get(e)->position.x == doctest::Approx(10.0f));
    CHECK_FALSE(anim.playing); // Once stopped
    // A further tick holds the end value (stopped).
    sceneObj.Update(1.0f);
    CHECK(targets->Get(e)->position.x == doctest::Approx(10.0f));
}

TEST_CASE("property animator: a track to a missing component/property is disabled, not fatal")
{
    EnsureReflected();
    scene::Scene sceneObj{DefaultAllocator()};
    sceneObj.AddSystem<AnimTargetManager>();
    sceneObj.AddSystem<PropertyAnimatorComponentManager>();
    auto* targets = sceneObj.GetSystem<AnimTargetManager>();
    auto* animators = sceneObj.GetSystem<PropertyAnimatorComponentManager>();

    const scene::EntityHandle e = sceneObj.CreateEntity(u8"Mixed");
    AnimTarget& target = targets->Add(e);

    // One good track (value), one bad (unknown property) and one bad (unknown component).
    RefPtr<PropertyAnimationClipResource> clip =
        MakeRef<PropertyAnimationClipResource>(DefaultAllocator());
    {
        PropertyTrack good;
        good.componentType = String(u8"AnimTarget");
        good.propertyPath = String(u8"value");
        good.kind = TrackValueKind::Float;
        CurveKey a; a.time = 0.0f; a.value = 0.0f;
        CurveKey b; b.time = 1.0f; b.value = 8.0f;
        good.channels[0].AddKey(a);
        good.channels[0].AddKey(b);
        clip->clip.tracks.PushBack(Move(good));

        PropertyTrack badProp;
        badProp.componentType = String(u8"AnimTarget");
        badProp.propertyPath = String(u8"nonesuch");
        badProp.kind = TrackValueKind::Float;
        clip->clip.tracks.PushBack(Move(badProp));

        PropertyTrack badComp;
        badComp.componentType = String(u8"NoSuchComponent");
        badComp.propertyPath = String(u8"x");
        badComp.kind = TrackValueKind::Float;
        clip->clip.tracks.PushBack(Move(badComp));
    }
    clip->clip.duration = clip->clip.ComputeDuration();

    PropertyAnimatorComponent& anim = animators->Add(e);
    anim.clip = clip.Get();

    sceneObj.Update(0.0f);
    sceneObj.Update(0.5f); // must not crash despite the two bad tracks
    CHECK(target.value == doctest::Approx(4.0f)); // the good track still animates
}

TEST_CASE("property animator: reflected play/pause/stop/setTime drive the clock")
{
    EnsureReflected();
    scene::Scene sceneObj{DefaultAllocator()};
    sceneObj.AddSystem<AnimTargetManager>();
    sceneObj.AddSystem<PropertyAnimatorComponentManager>();
    auto* targets = sceneObj.GetSystem<AnimTargetManager>();
    auto* animators = sceneObj.GetSystem<PropertyAnimatorComponentManager>();

    const scene::EntityHandle e = sceneObj.CreateEntity(u8"Scripted");
    AnimTarget& target = targets->Add(e);
    RefPtr<PropertyAnimationClipResource> clip = MakePositionClip();
    PropertyAnimatorComponent& anim = animators->Add(e);
    anim.clip = clip.Get();
    anim.autoplay = false; // driven by script-style method calls, not autoplay

    // No autoplay: the first tick binds but stays stopped.
    sceneObj.Update(0.0f);
    CHECK_FALSE(anim.isPlaying());
    CHECK(target.position.x == doctest::Approx(0.0f));

    // play() starts from 0; a half-second tick reaches the midpoint.
    anim.play();
    CHECK(anim.isPlaying());
    sceneObj.Update(0.5f);
    CHECK(target.position.x == doctest::Approx(5.0f));

    // pause() freezes the clock: the value holds across ticks.
    anim.pause();
    CHECK_FALSE(anim.isPlaying());
    sceneObj.Update(0.5f);
    CHECK(target.position.x == doctest::Approx(5.0f));

    // setTime and resume jump the clock.
    anim.setTime(0.8f);
    anim.resume();
    sceneObj.Update(0.0f);
    CHECK(target.position.x == doctest::Approx(8.0f));

    // stop() resets to 0.
    anim.stop();
    CHECK_FALSE(anim.isPlaying());
    CHECK(anim.currentTime() == doctest::Approx(0.0f));
}

TEST_CASE("property animator: entity-active - starts-inactive never advances; toggle freezes/resumes")
{
    EnsureReflected();
    scene::Scene sceneObj{DefaultAllocator()};
    sceneObj.AddSystem<AnimTargetManager>();
    sceneObj.AddSystem<PropertyAnimatorComponentManager>();
    auto* targets = sceneObj.GetSystem<AnimTargetManager>();
    auto* animators = sceneObj.GetSystem<PropertyAnimatorComponentManager>();

    const scene::EntityHandle e = sceneObj.CreateEntity(u8"Frozen");
    AnimTarget& target = targets->Add(e);
    RefPtr<PropertyAnimationClipResource> clip = MakePositionClip();
    PropertyAnimatorComponent& anim = animators->Add(e);
    anim.clip = clip.Get();
    anim.autoplay = true;

    // Inactive from the first tick (the scene-starts-inactive case): no bind, no writes.
    sceneObj.SetActive(e, false);
    sceneObj.Update(0.5f);
    sceneObj.Update(0.5f);
    CHECK(target.position.x == doctest::Approx(0.0f));

    // Activation: binds and autoplays from t=0 (nothing pre-advanced while dark).
    sceneObj.SetActive(e, true);
    sceneObj.Update(0.0f);
    sceneObj.Update(0.5f); // midpoint of the 0..10 ramp
    CHECK(target.position.x == doctest::Approx(5.0f));

    // Deactivate mid-clip: time freezes.
    sceneObj.SetActive(e, false);
    sceneObj.Update(0.3f);
    CHECK(target.position.x == doctest::Approx(5.0f));

    // Reactivate: resumes from where it froze.
    sceneObj.SetActive(e, true);
    sceneObj.Update(0.4f); // 0.5 + 0.4 = 0.9, so x ~ 9
    CHECK(target.position.x == doctest::Approx(9.0f));
}

TEST_CASE("property animator: SIMULATION-GATED - a non-simulating scene freezes; enabling resumes")
{
    // Animation does not advance in the editor's edit mode (scene simulation disabled). Scenes
    // default to simulating, so this is opt-out only.
    EnsureReflected();
    scene::Scene sceneObj{DefaultAllocator()};
    sceneObj.AddSystem<AnimTargetManager>();
    sceneObj.AddSystem<PropertyAnimatorComponentManager>();
    auto* targets = sceneObj.GetSystem<AnimTargetManager>();
    auto* animators = sceneObj.GetSystem<PropertyAnimatorComponentManager>();

    const scene::EntityHandle e = sceneObj.CreateEntity(u8"EditFrozen");
    AnimTarget& target = targets->Add(e);
    RefPtr<PropertyAnimationClipResource> clip = MakePositionClip();
    PropertyAnimatorComponent& anim = animators->Add(e);
    anim.clip = clip.Get();
    anim.autoplay = true;

    // Edit mode: simulation off, so the manager never binds, advances or writes.
    sceneObj.SetSimulationEnabled(false);
    sceneObj.Update(0.5f);
    sceneObj.Update(0.5f);
    CHECK(target.position.x == doctest::Approx(0.0f));

    // Simulate: binds and autoplays from t=0.
    sceneObj.SetSimulationEnabled(true);
    sceneObj.Update(0.0f);
    sceneObj.Update(0.5f);
    CHECK(target.position.x == doctest::Approx(5.0f));

    // Back to edit mode mid-clip: frozen where it was.
    sceneObj.SetSimulationEnabled(false);
    sceneObj.Update(0.4f);
    CHECK(target.position.x == doctest::Approx(5.0f));
}
