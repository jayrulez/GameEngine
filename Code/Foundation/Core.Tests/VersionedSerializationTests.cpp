// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Data-version scopes: payloads carry the writing type's data-version chain. The reader
// requires the stored chain to equal the type's current one. There is no migration, so a
// Serialize body reads exactly one layout and a stale payload fails instead of misparsing.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

import foundation.core;

using namespace foundation::core;

namespace
{
    struct Soldier
    {
        f32 health = 100.0f;
        f32 armor = 0.0f;
    };
    void Serialize(ISerializer& ar, Soldier& s)
    {
        foundation::core::Serialize(ar, "health", s.health);
        foundation::core::Serialize(ar, "armor", s.armor);
    }

    // Writes `count` chain entries by hand (a writer of some other build), then the payload.
    void WriteWithChain(MemoryStream& stream, const SerializedDataVersion* chain, u32 count,
                        const Soldier& value)
    {
        BinarySerializer ar(stream, SerializeMode::Write);
        u32 n = count;
        ar.Key("dataVersions");
        ar.BeginArray(n);
        for (u32 i = 0; i < count; ++i)
        {
            SerializedDataVersion entry = chain[i];
            ar.Key("type");
            ar.Scalar(&entry.typeId, ScalarKind::UInt64);
            ar.Key("version");
            ar.Scalar(&entry.version, ScalarKind::UInt32);
        }
        ar.EndArray();
        ar.PushVersionScope(chain, count);
        Soldier copy = value;
        Serialize(ar, copy);
        ar.PopVersionScope();
        REQUIRE(ar.IsOk());
    }
}

TEST_CASE("versioning: scopes stack and expose the concrete + base versions")
{
    MemoryStream stream;
    BinarySerializer ar(stream, SerializeMode::Write);
    CHECK(ar.Version() == 0u); // no scope

    const SerializedDataVersion outer[] = {{111u, 3u}, {222u, 5u}}; // concrete + base
    ar.PushVersionScope(outer, 2);
    CHECK(ar.Version() == 3u);
    CHECK(ar.Version(111u) == 3u);
    CHECK(ar.Version(222u) == 5u);
    CHECK(ar.Version(999u) == 0u); // not in the chain

    const SerializedDataVersion inner[] = {{333u, 7u}}; // nested object
    ar.PushVersionScope(inner, 1);
    CHECK(ar.Version() == 7u);
    CHECK(ar.Version(222u) == 0u); // outer scope masked while inner is active

    ar.PopVersionScope();
    CHECK(ar.Version() == 3u); // outer restored
    ar.PopVersionScope();
    CHECK(ar.Version() == 0u);
}

TEST_CASE("versioning: current-version data round-trips through BeginVersionedPayload")
{
    MemoryStream stream;
    {
        BinarySerializer ar(stream, SerializeMode::Write);
        BeginVersionedPayload(ar, TypeOf<Soldier>());
        Soldier s;
        s.health = 70.0f;
        s.armor = 25.0f;
        Serialize(ar, s);
        EndVersionedPayload(ar);
        REQUIRE(ar.IsOk());
    }
    REQUIRE(stream.Seek(0, SeekOrigin::Begin) == 0);
    {
        BinarySerializer ar(stream, SerializeMode::Read);
        BeginVersionedPayload(ar, TypeOf<Soldier>());
        CHECK(ar.Version() == TypeOf<Soldier>().dataVersion); // the verified current version
        Soldier s;
        Serialize(ar, s);
        EndVersionedPayload(ar);
        REQUIRE(ar.IsOk());
        CHECK(s.health == doctest::Approx(70.0f));
        CHECK(s.armor == doctest::Approx(25.0f));
    }
}

TEST_CASE("versioning: a payload stored under another data version is REFUSED, not migrated")
{
    // The type's current version is TypeOf<Soldier>().dataVersion (0: unregistered here); a
    // writer that stamped version 1 is a stale (or newer) build. The read fails at the header,
    // before any field is consumed, so no layout guess ever produces garbage.
    Soldier value;
    value.health = 40.0f;
    value.armor = 5.0f;
    const SerializedDataVersion stale[] = {{TypeOf<Soldier>().id, TypeOf<Soldier>().dataVersion + 1u}};
    MemoryStream stream;
    WriteWithChain(stream, stale, 1, value);
    REQUIRE(stream.Seek(0, SeekOrigin::Begin) == 0);

    BinarySerializer ar(stream, SerializeMode::Read);
    BeginVersionedPayload(ar, TypeOf<Soldier>());
    CHECK(!ar.IsOk());
    CHECK(ar.GetStatus().Code() == ErrorCode::NotSupported);
    Soldier loaded;
    Serialize(ar, loaded);
    EndVersionedPayload(ar);
    CHECK(!ar.IsOk());
}

TEST_CASE("versioning: a chain of a different SHAPE (extra or foreign entries) is refused too")
{
    Soldier value;
    const SerializedDataVersion foreign[] = {{TypeOf<Soldier>().id, TypeOf<Soldier>().dataVersion},
                                             {0x1234u, 2u}}; // a base this type never had
    MemoryStream stream;
    WriteWithChain(stream, foreign, 2, value);
    REQUIRE(stream.Seek(0, SeekOrigin::Begin) == 0);
    BinarySerializer ar(stream, SerializeMode::Read);
    BeginVersionedPayload(ar, TypeOf<Soldier>());
    CHECK(!ar.IsOk());

    const SerializedDataVersion other[] = {{0x5678u, TypeOf<Soldier>().dataVersion}}; // wrong type
    MemoryStream stream2;
    WriteWithChain(stream2, other, 1, value);
    REQUIRE(stream2.Seek(0, SeekOrigin::Begin) == 0);
    BinarySerializer ar2(stream2, SerializeMode::Read);
    BeginVersionedPayload(ar2, TypeOf<Soldier>());
    CHECK(!ar2.IsOk());
}

TEST_CASE("versioning: a type with a legacy reader accepts an older concrete version down to its floor, and reports it")
{
    // TypeInfo::minReadDataVersion (TypeBuilder::ReadsDataVersionsFrom): the one allowance in
    // the strict reader. Versions below the floor and above the current one are still refused;
    // a base entry must still match exactly.
    TypeInfo aware = MakeTypeInfo<Soldier>("SoldierLegacyAware", "rtti::test", nullptr, 3);
    KeepLegacyReader(aware, 2);
    Soldier value;
    value.health = 12.0f;

    const auto readWith = [&](u32 storedVersion, u32& reportedVersion) -> bool
    {
        const SerializedDataVersion chain[] = {{aware.id, storedVersion}};
        MemoryStream stream;
        WriteWithChain(stream, chain, 1, value);
        REQUIRE(stream.Seek(0, SeekOrigin::Begin) == 0);
        BinarySerializer ar(stream, SerializeMode::Read);
        BeginVersionedPayload(ar, aware);
        reportedVersion = ar.Version();
        Soldier loaded;
        Serialize(ar, loaded);
        EndVersionedPayload(ar);
        return ar.IsOk() && loaded.health == 12.0f;
    };
    u32 reported = 0;
    CHECK(readWith(3, reported)); // current
    CHECK(reported == 3u);
    CHECK(readWith(2, reported)); // the floor: accepted, and the body sees the stored version
    CHECK(reported == 2u);
    CHECK_FALSE(readWith(1, reported)); // below the floor
    CHECK_FALSE(readWith(4, reported)); // a newer build's data
    aware.readsLegacyData = false; // no legacy reader: the current version only
    CHECK_FALSE(readWith(2, reported));
    // A reader of never-versioned data: a floor of 0 is a floor too.
    KeepLegacyReader(aware, 0);
    CHECK(readWith(0, reported));
    CHECK(reported == 0u);
    CHECK(readWith(2, reported));
}
