// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// foundation.propertyanimation.resource - the cooked-clip wire: FromClip flattens, the clip is
// serialized to binary and back, and FillClip rebuilds it; with count guards on the pools and the
// same samples before and after.

#include <doctest/doctest.h>
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"
#include <initializer_list>

import foundation.core;
import foundation.propertyanimation;
import foundation.propertyanimation.resource;

using namespace foundation::core;
using namespace foundation::propertyanimation;

namespace
{
    CurveKey K(f32 t, f32 v, CurveKeyInterpolation interp = CurveKeyInterpolation::Linear, f32 tin = 0.0f,
               f32 tout = 0.0f)
    {
        CurveKey k;
        k.time = t;
        k.value = v;
        k.interpolation = interp;
        k.tangentIn = tin;
        k.tangentOut = tout;
        return k;
    }

    // A representative clip: a Float3 position track (3 channels), a Color track (4 channels), and a
    // Quat rotation track, exercising every kind and the channel-cursor reconstruction.
    PropertyAnimationClip MakeClip()
    {
        PropertyAnimationClip clip;

        PropertyTrack pos;
        pos.componentType = String(u8"Transform");
        pos.propertyPath = String(u8"position");
        pos.kind = TrackValueKind::Float3;
        pos.channels[0].AddKey(K(0.0f, 0.0f, CurveKeyInterpolation::Cubic, 1.0f, 2.0f));
        pos.channels[0].AddKey(K(2.0f, 20.0f));
        pos.channels[1].AddKey(K(0.0f, 5.0f));
        pos.channels[2].AddKey(K(0.0f, -1.0f, CurveKeyInterpolation::Constant));
        pos.channels[2].AddKey(K(2.0f, 1.0f));
        clip.tracks.PushBack(Move(pos));

        PropertyTrack col;
        col.componentType = String(u8"Light");
        col.propertyPath = String(u8"light.color");
        col.kind = TrackValueKind::Color;
        for (u32 c = 0; c < 4; ++c)
        {
            col.channels[c].AddKey(K(0.0f, 0.0f));
            col.channels[c].AddKey(K(1.0f, 0.25f * static_cast<f32>(c + 1)));
        }
        clip.tracks.PushBack(Move(col));

        PropertyTrack rot;
        rot.componentType = String(u8"Transform");
        rot.propertyPath = String(u8"rotation");
        rot.kind = TrackValueKind::Quat;
        rot.quatKeys.PushBack(QuatKey{0.0f, Quaternion::Identity});
        rot.quatKeys.PushBack(
            QuatKey{1.0f, Quaternion::FromAxisAngle(Float3{0, 1, 0}, 1.0f)});
        clip.tracks.PushBack(Move(rot));

        clip.duration = clip.ComputeDuration();
        return clip;
    }
}

TEST_CASE("propanim.resource: cooked-clip wire round-trips (counts + sample identity)")
{
    GlobalTypeRegistry().Register(PropertyAnimationClipSource::StaticType());
    RegisterSerializable<PropertyAnimationClipSource>();

    const PropertyAnimationClip original = MakeClip();

    // Flatten, then serialize to binary.
    PropertyAnimationClipSource src;
    PropertyAnimationClipSource::FromClip(original, src);
    // Count guards: 3 tracks, channel entries = 3 (Float3) + 4 (Color) + 0 (Quat) = 7, key pool =
    // (2+1+2) + (2*4) = 13, quat pool = 2.
    CHECK(src.trackKind.Size() == 3u);
    CHECK(src.channelKeyStart.Size() == 7u);
    CHECK(src.channelKeyCount.Size() == 7u);
    CHECK(src.keyTime.Size() == 13u);
    CHECK(src.quatValue.Size() == 2u);
    CHECK(src.trackQuatStart.Size() == 3u);

    MemoryStream stream;
    {
        BinarySerializer writer(stream, SerializeMode::Write);
        src.Serialize(writer);
    }
    (void)stream.Seek(0, SeekOrigin::Begin);

    PropertyAnimationClipSource restoredSrc;
    {
        BinarySerializer reader(stream, SerializeMode::Read);
        restoredSrc.Serialize(reader);
    }

    // Deserialize, then rebuild the runtime clip.
    PropertyAnimationClip rebuilt;
    restoredSrc.FillClip(rebuilt);

    REQUIRE(rebuilt.tracks.Size() == original.tracks.Size());
    CHECK(rebuilt.duration == doctest::Approx(original.duration));

    // Track metadata and kinds survived.
    CHECK(rebuilt.tracks[0].componentType.AsView() == StringView(u8"Transform"));
    CHECK(rebuilt.tracks[0].propertyPath.AsView() == StringView(u8"position"));
    CHECK(rebuilt.tracks[0].kind == TrackValueKind::Float3);
    CHECK(rebuilt.tracks[1].propertyPath.AsView() == StringView(u8"light.color"));
    CHECK(rebuilt.tracks[2].kind == TrackValueKind::Quat);

    // The same samples at a few times across every kind (curves, tangents and quats all preserved).
    for (const f32 t : {0.0f, 0.5f, 1.0f, 1.5f, 2.0f})
    {
        const Float3 a = original.tracks[0].Sample(t).Get<Float3>();
        const Float3 b = rebuilt.tracks[0].Sample(t).Get<Float3>();
        CHECK(a.x == doctest::Approx(b.x));
        CHECK(a.y == doctest::Approx(b.y));
        CHECK(a.z == doctest::Approx(b.z));

        const Color ca = original.tracks[1].Sample(t).Get<Color>();
        const Color cb = rebuilt.tracks[1].Sample(t).Get<Color>();
        CHECK(ca.r == doctest::Approx(cb.r));
        CHECK(ca.a == doctest::Approx(cb.a));

        const Quaternion qa = original.tracks[2].Sample(t).Get<Quaternion>();
        const Quaternion qb = rebuilt.tracks[2].Sample(t).Get<Quaternion>();
        CHECK(qa.w == doctest::Approx(qb.w));
        CHECK(qa.y == doctest::Approx(qb.y));
    }

    // The interpolation mode on the first key survived (Cubic), not silently reset to Linear.
    REQUIRE(rebuilt.tracks[0].channels[0].KeyCount() == 2u);
    CHECK(rebuilt.tracks[0].channels[0].Keys()[0].interpolation == CurveKeyInterpolation::Cubic);
    CHECK(rebuilt.tracks[0].channels[0].Keys()[0].tangentOut == doctest::Approx(2.0f));
}

TEST_CASE("propanim.resource: a malformed wire yields a safe partial clip, never an overread")
{
    // trackKind says 1 track but the pools are empty: FillClip must bound every index.
    PropertyAnimationClipSource src;
    src.trackKind.PushBack(static_cast<u8>(TrackValueKind::Float3));
    src.trackComponent.PushBack(String(u8"C"));
    src.trackPath.PushBack(String(u8"p"));
    // channelKeyStart claims a key run past the (empty) key pool.
    src.channelKeyStart.PushBack(0u);
    src.channelKeyCount.PushBack(99u);

    PropertyAnimationClip clip;
    src.FillClip(clip); // must not crash or overread
    REQUIRE(clip.tracks.Size() == 1u);
    CHECK(clip.tracks[0].channels[0].KeyCount() == 0u); // no keys materialized from the bad run
}

TEST_CASE("propanim.resource: a key's tangent mode round-trips; a wire without modes reads Broken")
{
    PropertyAnimationClip original = MakeClip();
    original.tracks[0].channels[0].Keys()[0].tangentMode = CurveTangentMode::Auto;
    original.tracks[0].channels[1].Keys()[0].tangentMode = CurveTangentMode::Flat;
    PropertyAnimationClipSource src;
    PropertyAnimationClipSource::FromClip(original, src);
    CHECK(src.keyTangentMode.Size() == src.keyTime.Size());

    PropertyAnimationClip rebuilt;
    src.FillClip(rebuilt);
    REQUIRE(rebuilt.tracks.Size() == original.tracks.Size());
    CHECK(rebuilt.tracks[0].channels[0].Keys()[0].tangentMode == CurveTangentMode::Auto);
    CHECK(rebuilt.tracks[0].channels[1].Keys()[0].tangentMode == CurveTangentMode::Flat);

    // A clip saved before modes: every key keeps its tangents as stored.
    src.keyTangentMode.Clear();
    PropertyAnimationClip older;
    src.FillClip(older);
    CHECK(older.tracks[0].channels[0].Keys()[0].tangentMode == CurveTangentMode::Broken);
    CHECK(older.tracks[0].channels[0].Keys()[0].tangentOut == doctest::Approx(2.0f));
}
