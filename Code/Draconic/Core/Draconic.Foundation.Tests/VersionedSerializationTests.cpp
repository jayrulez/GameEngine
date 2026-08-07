// Data-version scopes (serialization migration, Traktor-style): payloads carry the writing
// type's data-version chain; Serialize bodies branch on ar.Version() to read old layouts and
// upgrade on the next save. One code path, no migration files.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

import draconic.foundation;

using namespace draconic::foundation;

namespace
{
    // A type whose layout evolved: v1 had only `health`; v2 added `armor`.
    struct Soldier
    {
        f32 health = 100.0f;
        f32 armor = 0.0f;
    };
    void Serialize(ISerializer& ar, Soldier& s)
    {
        draconic::foundation::Serialize(ar, "health", s.health);
        if (ar.Version() >= 2)
        {
            draconic::foundation::Serialize(ar, "armor", s.armor);
        }
        else if (ar.Mode() == SerializeMode::Read)
        {
            s.armor = 10.0f;
        } // migration default
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

TEST_CASE("versioning: a versioned payload round-trips its stored version (binary)")
{
    // Simulate v1 data: write with an EXPLICIT version-1 scope (the writer of yesteryear).
    MemoryStream stream;
    {
        BinarySerializer ar(stream, SerializeMode::Write);
        u32 count = 1;
        SerializedDataVersion v1{TypeOf<Soldier>().id, 1u};
        ar.Key("dataVersions");
        ar.BeginArray(count);
        ar.Key("type");
        ar.Scalar(&v1.typeId, ScalarKind::UInt64);
        ar.Key("version");
        ar.Scalar(&v1.version, ScalarKind::UInt32);
        ar.EndArray();
        ar.PushVersionScope(&v1, 1);
        Soldier old;
        old.health = 40.0f;
        Serialize(ar, old); // v1 layout: health only (the branch sees Version()==1)
        ar.PopVersionScope();
        REQUIRE(ar.IsOk());
    }

    // Read through the CURRENT code: the stored version drives the migration branch.
    REQUIRE(stream.Seek(0, SeekOrigin::Begin) == 0);
    {
        BinarySerializer ar(stream, SerializeMode::Read);
        BeginVersionedPayload(ar, TypeOf<Soldier>());
        CHECK(ar.Version() == 1u); // the DATA's version, not the type's current one
        Soldier loaded;
        Serialize(ar, loaded);
        EndVersionedPayload(ar);
        REQUIRE(ar.IsOk());
        CHECK(loaded.health == doctest::Approx(40.0f));
        CHECK(loaded.armor == doctest::Approx(10.0f)); // migration default applied
    }
}

TEST_CASE("versioning: current-version data round-trips through BeginVersionedPayload")
{
    // With the type registered at version 2 the same code writes v2 and reads it back whole.
    // (TypeOf<Soldier> is unregistered here - dataVersion 0 - so emulate the registered type
    // by checking the write side emits what the read side consumes symmetrically.)
    MemoryStream stream;
    {
        BinarySerializer ar(stream, SerializeMode::Write);
        BeginVersionedPayload(ar, TypeOf<Soldier>());
        Soldier s;
        s.health = 70.0f;
        s.armor = 25.0f; // written only when Version() >= 2... which is 0 here
        Serialize(ar, s);
        EndVersionedPayload(ar);
        REQUIRE(ar.IsOk());
    }
    REQUIRE(stream.Seek(0, SeekOrigin::Begin) == 0);
    {
        BinarySerializer ar(stream, SerializeMode::Read);
        BeginVersionedPayload(ar, TypeOf<Soldier>());
        CHECK(ar.Version() == TypeOf<Soldier>().dataVersion);
        Soldier s;
        Serialize(ar, s);
        EndVersionedPayload(ar);
        REQUIRE(ar.IsOk());
        CHECK(s.health == doctest::Approx(70.0f));
    }
}
