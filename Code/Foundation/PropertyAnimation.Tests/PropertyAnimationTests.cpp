// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// foundation.propertyanimation - evaluation (track Sample into Variants, quat slerp, durations) and
// the reflection binding resolver (nested paths, missing or invalid targets, the re-walk on write).

#include <doctest/doctest.h>
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

import foundation.core;
import foundation.propertyanimation;

using namespace foundation::core;
using namespace foundation::propertyanimation;

namespace
{
    // A nested struct on the "component" (the light.tint path).
    class AnimLight : public Object
    {
        RTTI_OBJECT(AnimLight, Object)
    public:
        Color tint{0.0f, 0.0f, 0.0f, 1.0f};
    };

    // A component-like reflected object with animatable leaf and nested properties.
    class AnimComp : public Object
    {
        RTTI_OBJECT(AnimComp, Object)
    public:
        Float3 position{0.0f, 0.0f, 0.0f};
        Quaternion rotation = Quaternion::Identity;
        f32 intensity = 0.0f;
        AnimLight light;
    };

    CurveKey K(f32 t, f32 v)
    {
        CurveKey k;
        k.time = t;
        k.value = v;
        return k;
    }

    void EnsureRegistered()
    {
        static bool done = false;
        if (done)
        {
            return;
        }
        done = true;
        RegisterCoreTypes(); // Float3 / Color / Quaternion as Variant-usable reflected values
        GlobalTypeRegistry().Register(AnimLight::StaticType());
        GlobalTypeRegistry().Register(AnimComp::StaticType());
    }
}

REFLECT_MEMBERS(AnimLight, "rtti::propanim::test") { builder.Property<&AnimLight::tint>("tint"); }

REFLECT_MEMBERS(AnimComp, "rtti::propanim::test")
{
    builder.Property<&AnimComp::position>("position")
        .Property<&AnimComp::rotation>("rotation")
        .Property<&AnimComp::intensity>("intensity")
        .Nested<&AnimComp::light>("light");
}

TEST_CASE("propanim: Float track samples a scalar Variant")
{
    EnsureRegistered();
    PropertyTrack t;
    t.kind = TrackValueKind::Float;
    t.channels[0].AddKey(K(0.0f, 0.0f));
    t.channels[0].AddKey(K(1.0f, 10.0f));

    CHECK(t.Sample(0.0f).Get<f32>() == doctest::Approx(0.0f));
    CHECK(t.Sample(0.5f).Get<f32>() == doctest::Approx(5.0f));
    CHECK(t.Sample(1.0f).Get<f32>() == doctest::Approx(10.0f));
    CHECK(t.Duration() == doctest::Approx(1.0f));
}

TEST_CASE("propanim: Float3 track samples per-component curves")
{
    EnsureRegistered();
    PropertyTrack t;
    t.kind = TrackValueKind::Float3;
    t.channels[0].AddKey(K(0.0f, 0.0f));
    t.channels[0].AddKey(K(2.0f, 20.0f));
    t.channels[1].AddKey(K(0.0f, 5.0f)); // constant-ish (single key)
    t.channels[2].AddKey(K(0.0f, -1.0f));
    t.channels[2].AddKey(K(2.0f, 1.0f));

    const Float3 v = t.Sample(1.0f).Get<Float3>();
    CHECK(v.x == doctest::Approx(10.0f)); // half of 0..20
    CHECK(v.y == doctest::Approx(5.0f));  // single key holds
    CHECK(v.z == doctest::Approx(0.0f));  // midpoint of -1..1
    CHECK(t.Duration() == doctest::Approx(2.0f));
}

TEST_CASE("propanim: Color track samples RGBA channels")
{
    EnsureRegistered();
    PropertyTrack t;
    t.kind = TrackValueKind::Color;
    for (u32 c = 0; c < 4; ++c)
    {
        t.channels[c].AddKey(K(0.0f, 0.0f));
        t.channels[c].AddKey(K(1.0f, static_cast<f32>(c + 1) * 0.25f));
    }
    const Color col = t.Sample(1.0f).Get<Color>();
    CHECK(col.r == doctest::Approx(0.25f));
    CHECK(col.g == doctest::Approx(0.50f));
    CHECK(col.b == doctest::Approx(0.75f));
    CHECK(col.a == doctest::Approx(1.00f));
}

TEST_CASE("propanim: Quat track slerps shortest-arc between keys")
{
    EnsureRegistered();
    PropertyTrack t;
    t.kind = TrackValueKind::Quat;
    t.quatKeys.PushBack(QuatKey{0.0f, Quaternion::Identity});
    // 90 deg about Y.
    const Quaternion y90 = Quaternion::FromAxisAngle(Float3{0.0f, 1.0f, 0.0f}, 1.57079633f);
    t.quatKeys.PushBack(QuatKey{1.0f, y90});

    // Endpoints exact; midpoint ~45 deg (its y component between 0 and sin(45)).
    const Quaternion at0 = t.Sample(0.0f).Get<Quaternion>();
    CHECK(at0.w == doctest::Approx(1.0f));
    const Quaternion mid = t.Sample(0.5f).Get<Quaternion>();
    CHECK(mid.y > 0.0f);
    CHECK(mid.y < y90.y);       // between identity and 90deg
    CHECK(mid.w < 1.0f);        // rotated away from identity
    CHECK(t.Duration() == doctest::Approx(1.0f));
}

TEST_CASE("propanim: ResolveBinding - leaf, nested, and the failure cases")
{
    EnsureRegistered();
    const TypeInfo& type = AnimComp::StaticType();

    // Leaf property: one link.
    const PropertyBinding pos = ResolveBinding(type, u8"position");
    REQUIRE(pos.IsResolved());
    CHECK(pos.chain.Size() == 1u);

    // Nested path: two links (light -> tint).
    const PropertyBinding tint = ResolveBinding(type, u8"light.tint");
    REQUIRE(tint.IsResolved());
    CHECK(tint.chain.Size() == 2u);

    // Missing property, empty path, and a non-nested intermediate all fail (unresolved).
    CHECK_FALSE(ResolveBinding(type, u8"missing").IsResolved());
    CHECK_FALSE(ResolveBinding(type, u8"").IsResolved());
    CHECK_FALSE(ResolveBinding(type, u8"position.x").IsResolved()); // position is a leaf, not Nested
    CHECK_FALSE(ResolveBinding(type, u8"light.missing").IsResolved());
}

TEST_CASE("propanim: WriteBinding writes leaf + nested properties through reflection")
{
    EnsureRegistered();
    AnimComp comp;
    const Instance inst = Instance::From(&comp);

    // Leaf Float3.
    const PropertyBinding pos = ResolveBinding(AnimComp::StaticType(), u8"position");
    REQUIRE(WriteBinding(pos, inst, Variant::From(Float3{1.0f, 2.0f, 3.0f})).IsOk());
    CHECK(comp.position.x == doctest::Approx(1.0f));
    CHECK(comp.position.y == doctest::Approx(2.0f));
    CHECK(comp.position.z == doctest::Approx(3.0f));

    // Nested Color (light.tint).
    const PropertyBinding tint = ResolveBinding(AnimComp::StaticType(), u8"light.tint");
    REQUIRE(WriteBinding(tint, inst, Variant::From(Color{0.5f, 0.25f, 0.125f, 1.0f})).IsOk());
    CHECK(comp.light.tint.r == doctest::Approx(0.5f));
    CHECK(comp.light.tint.g == doctest::Approx(0.25f));
    CHECK(comp.light.tint.b == doctest::Approx(0.125f));

    // A second write re-walks the address (never caches the sub-instance pointer).
    REQUIRE(WriteBinding(tint, inst, Variant::From(Color{0.9f, 0.0f, 0.0f, 1.0f})).IsOk());
    CHECK(comp.light.tint.r == doctest::Approx(0.9f));

    // An unresolved binding is a clean InvalidArgument, never a crash.
    CHECK_FALSE(WriteBinding(ResolveBinding(AnimComp::StaticType(), u8"nope"), inst,
                             Variant::From(1.0f))
                    .IsOk());
}

TEST_CASE("propanim: ReadBinding is the symmetric inverse of WriteBinding (snapshot round-trip)")
{
    EnsureRegistered();
    AnimComp comp;
    comp.position = Float3{7.0f, 8.0f, 9.0f};
    comp.light.tint = Color{0.2f, 0.3f, 0.4f, 1.0f};
    const Instance inst = Instance::From(&comp);

    // Read the pre-write leaf and nested values (the preview snapshot).
    const PropertyBinding pos = ResolveBinding(AnimComp::StaticType(), u8"position");
    const PropertyBinding tint = ResolveBinding(AnimComp::StaticType(), u8"light.tint");
    const Variant posSnapshot = ReadBinding(pos, inst);
    const Variant tintSnapshot = ReadBinding(tint, inst);
    CHECK(posSnapshot.Get<Float3>().x == doctest::Approx(7.0f));
    CHECK(tintSnapshot.Get<Color>().g == doctest::Approx(0.3f));

    // A transient write (a preview), then a restore from the snapshot back to the originals.
    REQUIRE(WriteBinding(pos, inst, Variant::From(Float3{-1.0f, -1.0f, -1.0f})).IsOk());
    CHECK(comp.position.x == doctest::Approx(-1.0f));
    REQUIRE(WriteBinding(pos, inst, posSnapshot).IsOk());
    CHECK(comp.position.x == doctest::Approx(7.0f));
    CHECK(comp.position.z == doctest::Approx(9.0f));

    // An unresolved binding reads an empty Variant, never crashes.
    CHECK(ReadBinding(ResolveBinding(AnimComp::StaticType(), u8"nope"), inst).IsEmpty());
}

TEST_CASE("propanim: SampleMerged keeps the current value for empty channels (no teleport)")
{
    EnsureRegistered();
    PropertyTrack t;
    t.kind = TrackValueKind::Float3;
    // Only channel 0 (x) is keyed (0 -> 10 over 1s); y and z have no keys.
    t.channels[0].AddKey(K(0.0f, 0.0f));
    t.channels[0].AddKey(K(1.0f, 10.0f));

    // x is driven; y/z keep the live value instead of zeroing.
    const Float3 v = t.SampleMerged(0.5f, Variant::From(Float3{5.0f, 6.0f, 7.0f})).Get<Float3>();
    CHECK(v.x == doctest::Approx(5.0f));
    CHECK(v.y == doctest::Approx(6.0f));
    CHECK(v.z == doctest::Approx(7.0f));

    // A track with all channels empty returns `current` unchanged (a no-op write, no teleport).
    PropertyTrack empty;
    empty.kind = TrackValueKind::Float3;
    const Float3 same =
        empty.SampleMerged(0.5f, Variant::From(Float3{1.0f, 2.0f, 3.0f})).Get<Float3>();
    CHECK(same.x == doctest::Approx(1.0f));
    CHECK(same.y == doctest::Approx(2.0f));
    CHECK(same.z == doctest::Approx(3.0f));

    // Empty current (unreadable target) falls back to 0 for the empty channels.
    const Float3 fallback = t.SampleMerged(1.0f, Variant{}).Get<Float3>();
    CHECK(fallback.x == doctest::Approx(10.0f));
    CHECK(fallback.y == doctest::Approx(0.0f));
}

TEST_CASE("propanim: clip duration = the longest track")
{
    EnsureRegistered();
    PropertyAnimationClip clip;
    PropertyTrack a;
    a.kind = TrackValueKind::Float;
    a.channels[0].AddKey(K(0.0f, 0.0f));
    a.channels[0].AddKey(K(1.5f, 1.0f));
    PropertyTrack b;
    b.kind = TrackValueKind::Float;
    b.channels[0].AddKey(K(0.0f, 0.0f));
    b.channels[0].AddKey(K(3.0f, 1.0f));
    clip.tracks.PushBack(Move(a));
    clip.tracks.PushBack(Move(b));
    CHECK(clip.ComputeDuration() == doctest::Approx(3.0f));
}
