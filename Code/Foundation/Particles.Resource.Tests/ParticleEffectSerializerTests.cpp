// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The effect serializer refuses a module type this build cannot construct. A module's
// parameters sit in the positional binary stream behind its type id; "skipping" an unknown
// module would leave them there and shift everything after it (the next system's particle
// budget read as garbage - a runaway allocation). Strict, like a stale data version.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"
import foundation.core;
import foundation.particles;
import foundation.particles.resource;

using namespace foundation::core;
using namespace foundation::particles;

namespace
{
    void BuildEffect(ParticleEffect& fx)
    {
        ParticleSystem& first = fx.AddSystem(500, 7ull);
        first.name = String(u8"first");
        first.AddInitializer<LifetimeInitializer>().lifetime = RangeFloat(1.0f, 2.0f);
        first.AddBehavior<GravityBehavior>().multiplier = 2.0f;
        first.AddBehavior<AlphaOverLifetimeBehavior>().curve = ParticleCurveFloat::FadeOut(1.0f, 0.4f);

        ParticleSystem& second = fx.AddSystem(300, 9ull);
        second.name = String(u8"second");
        second.AddInitializer<LifetimeInitializer>().lifetime = RangeFloat(0.5f, 1.0f);
    }

    Array<u8> WriteEffect(ParticleEffect& fx)
    {
        MemoryStream buffer(DefaultAllocator());
        {
            BinarySerializer writer(buffer, SerializeMode::Write);
            SerializeEffect(writer, fx);
        }
        const Span<const byte> bytes = buffer.Bytes();
        Array<u8> out(DefaultAllocator());
        out.Resize(bytes.Size());
        for (usize i = 0; i < bytes.Size(); ++i)
        {
            out[i] = static_cast<u8>(bytes[i]);
        }
        return out;
    }

    /// Overwrite the first occurrence of the little-endian u64 `from` with `to`; false if absent.
    bool PatchU64(Array<u8>& bytes, u64 from, u64 to)
    {
        u8 pattern[8];
        u8 replacement[8];
        for (usize i = 0; i < 8; ++i)
        {
            pattern[i] = static_cast<u8>((from >> (8 * i)) & 0xFFu);
            replacement[i] = static_cast<u8>((to >> (8 * i)) & 0xFFu);
        }
        for (usize at = 0; at + 8 <= bytes.Size(); ++at)
        {
            bool match = true;
            for (usize i = 0; i < 8 && match; ++i)
            {
                match = bytes[at + i] == pattern[i];
            }
            if (match)
            {
                for (usize i = 0; i < 8; ++i)
                {
                    bytes[at + i] = replacement[i];
                }
                return true;
            }
        }
        return false;
    }

    bool ReadEffect(const Array<u8>& bytes, ParticleEffect& into)
    {
        MemoryStream buffer(DefaultAllocator());
        (void)buffer.Write(bytes.Data(), bytes.Size());
        (void)buffer.Seek(0, SeekOrigin::Begin);
        BinarySerializer reader(buffer, SerializeMode::Read);
        SerializeEffect(reader, into);
        return reader.IsPayloadOk();
    }
}

TEST_CASE("particles.serializer: a clean stream round-trips (control)")
{
    RegisterParticleEffectResource();
    ParticleEffectResource src;
    BuildEffect(src.Effect());
    const Array<u8> bytes = WriteEffect(src.Effect());

    ParticleEffectResource dst;
    CHECK(ReadEffect(bytes, dst.Effect()));
    REQUIRE(dst.Effect().SystemCount() == 2);
    CHECK(dst.Effect().GetSystem(0)->BehaviorCount() == 2);
    CHECK(dst.Effect().GetSystem(1)->MaxParticles() == 300);
}

TEST_CASE("particles.serializer: read over an effect, it replaces what the effect had (an editor's undo)")
{
    RegisterParticleEffectResource();
    ParticleEffectResource src;
    BuildEffect(src.Effect());
    SubEmitterLink link;
    link.childSystemIndex = 1;
    src.Effect().GetSystem(0)->subEmitters.PushBack(link);
    const Array<u8> bytes = WriteEffect(src.Effect());

    // The same effect read back over itself, as a whole-asset undo does: two systems, one link.
    CHECK(ReadEffect(bytes, src.Effect()));
    CHECK(src.Effect().SystemCount() == 2);
    CHECK(src.Effect().GetSystem(0)->subEmitters.Size() == 1u);
    CHECK(src.Effect().GetSystem(1)->subEmitters.IsEmpty());
}

TEST_CASE("particles.serializer: the effect-level links of before are read onto every system but the one each spawns into")
{
    RegisterParticleEffectResource();
    // The old layout by hand: three systems, then the effect's links (one into the third, one
    // into nothing).
    MemoryStream buffer(DefaultAllocator());
    {
        BinarySerializer writer(buffer, SerializeMode::Write);
        ParticleEffect fx;
        BuildEffect(fx);
        (void)fx.AddSystem(100);
        // The systems as they were written then: no sub-emitters of their own.
        foundation::core::Serialize(writer, "name", fx.name);
        u32 systems = static_cast<u32>(fx.SystemCount());
        writer.Key("systems");
        writer.BeginArray(systems);
        for (u32 i = 0; i < systems; ++i)
        {
            writer.BeginObject();
            SerializeSystem(writer, fx, static_cast<i32>(i), true);
            writer.EndObject();
        }
        writer.EndArray();
        u32 count = 2;
        writer.Key("links");
        writer.BeginArray(count);
        constexpr i32 kChildren[] = {2, -1};
        for (const i32 child : kChildren)
        {
            writer.BeginObject();
            SubEmitterLink link;
            link.childSystemIndex = child;
            link.spawnCount = 7;
            Serialize(writer, link);
            writer.EndObject();
        }
        writer.EndArray();
    }
    (void)buffer.Seek(0, SeekOrigin::Begin);
    BinarySerializer reader(buffer, SerializeMode::Read);
    ParticleEffect read;
    SerializeEffect(reader, read, true);
    REQUIRE(reader.IsPayloadOk());
    REQUIRE(read.SystemCount() == 3);
    // As it behaved: every other system's events spawned into the third.
    REQUIRE(read.GetSystem(0)->subEmitters.Size() == 1u);
    CHECK(read.GetSystem(0)->subEmitters[0].childSystemIndex == 2);
    CHECK(read.GetSystem(0)->subEmitters[0].spawnCount == 7);
    CHECK(read.GetSystem(1)->subEmitters.Size() == 1u);
    CHECK(read.GetSystem(2)->subEmitters.IsEmpty()); // never into itself; the link into nothing dropped
}

TEST_CASE("particles.serializer: a module type this build cannot construct refuses the payload")
{
    RegisterParticleEffectResource();
    ParticleEffectResource src;
    BuildEffect(src.Effect());
    Array<u8> bytes = WriteEffect(src.Effect());

    // Turn the first system's GravityBehavior into a type id nobody registered.
    constexpr u64 kUnknownTypeId = 0x00DEADBEEFCAFE01ull;
    REQUIRE(PatchU64(bytes, GravityBehavior::StaticType().id, kUnknownTypeId));

    ParticleEffectResource dst;
    CHECK_FALSE(ReadEffect(bytes, dst.Effect()));
    // Nothing after the unknown module was read from the shifted stream: the second system's
    // budget (which would have come back as garbage) was never added.
    CHECK(dst.Effect().SystemCount() <= 1);
    if (dst.Effect().SystemCount() == 1)
    {
        CHECK(dst.Effect().GetSystem(0)->MaxParticles() == 500);
        CHECK(dst.Effect().GetSystem(0)->BehaviorCount() == 0);
    }
}

TEST_CASE("particles.serializer: a registered type that is not a module refuses the payload too")
{
    RegisterParticleEffectResource();
    ParticleEffectResource src;
    BuildEffect(src.Effect());
    Array<u8> bytes = WriteEffect(src.Effect());

    // An initializer slot pointing at a behavior type: constructible, but not the right kind.
    REQUIRE(PatchU64(bytes, LifetimeInitializer::StaticType().id, GravityBehavior::StaticType().id));
    ParticleEffectResource dst;
    CHECK_FALSE(ReadEffect(bytes, dst.Effect()));
}
