// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// SchemaRecorder: what a Serialize body writes, recorded in order with kinds, defaults, nesting,
// text, guids, blobs and the versioned payload's chain: the wire format taken from the writer.

#include <doctest/doctest.h>
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

import foundation.core;

using namespace foundation::core;

namespace
{
    enum class Motion : u8
    {
        Static = 0,
        Kinematic = 1,
        Dynamic = 2
    };
    struct Inner
    {
        i32 depth = 3;
        bool lit = false;
    };
    void Serialize(ISerializer& ar, Inner& v)
    {
        ar.BeginObject();
        foundation::core::Serialize(ar, "depth", v.depth);
        foundation::core::Serialize(ar, "lit", v.lit);
        ar.EndObject();
    }
    struct Probe
    {
        Motion motion = Motion::Dynamic;
        f32 speed = 1.5f;
        bool on = true;
        i64 big = -7;
        u16 small = 65535;
        String name{u8"lamp"};
        Guid id;
        Inner inner;
        Array<f32> weights;
        Array<Inner> items;
        Array<u8> bytes;
        Probe()
        {
            REQUIRE(Guid::TryParse(u8"855ffed4-4da7-4fa0-9756-a95c6c842890", id));
            weights.PushBack(0.25f);
            weights.PushBack(0.75f);
            items.Resize(2);
            bytes.Resize(5);
        }
    };
    // The body writes an enum as a raw byte (RigidBody's shape), the way real components do.
    void Serialize(ISerializer& ar, Probe& p)
    {
        u8 motion = static_cast<u8>(p.motion);
        foundation::core::Serialize(ar, "motion", motion);
        p.motion = static_cast<Motion>(motion);
        foundation::core::Serialize(ar, "speed", p.speed);
        foundation::core::Serialize(ar, "on", p.on);
        foundation::core::Serialize(ar, "big", p.big);
        foundation::core::Serialize(ar, "small", p.small);
        foundation::core::Serialize(ar, "name", p.name);
        foundation::core::Serialize(ar, "id", p.id);
        foundation::core::Serialize(ar, "inner", p.inner);
        foundation::core::Serialize(ar, "weights", p.weights);
        foundation::core::Serialize(ar, "items", p.items);
        ar.Key("bytes");
        ar.Blob(p.bytes.Data(), p.bytes.Size());
    }
}

TEST_CASE("schema-recorder: a body's keys, kinds, defaults and nesting are recorded in write order")
{
    SchemaRecorder ar(DefaultAllocator());
    CHECK(ar.Mode() == SerializeMode::Write);
    Probe probe;
    Serialize(ar, probe);
    CHECK(ar.IsOk());
    const SchemaNode& root = ar.Root();
    CHECK(root.kind == SchemaNodeKind::Object);
    REQUIRE(root.children.Size() == 11u);

    // Order is the writer's order, and the enum is what the writer wrote: a u8 with value 2.
    CHECK(root.At(0)->key == u8"motion");
    CHECK(root.At(0)->kind == SchemaNodeKind::Scalar);
    CHECK(root.At(0)->scalar == ScalarKind::UInt8);
    CHECK(root.At(0)->value == u8"2");
    CHECK(root.At(1)->key == u8"speed");
    CHECK(root.At(1)->scalar == ScalarKind::Float32);
    CHECK(root.At(1)->value == u8"1.5");
    CHECK(root.Find(u8"on")->scalar == ScalarKind::Bool);
    CHECK(root.Find(u8"on")->value == u8"true");
    CHECK(root.Find(u8"big")->scalar == ScalarKind::Int64);
    CHECK(root.Find(u8"big")->value == u8"-7");
    CHECK(root.Find(u8"small")->scalar == ScalarKind::UInt16);
    CHECK(root.Find(u8"small")->value == u8"65535");
    // Text and guid are first class.
    CHECK(root.Find(u8"name")->kind == SchemaNodeKind::Text);
    CHECK(root.Find(u8"name")->value == u8"lamp");
    CHECK(root.Find(u8"id")->kind == SchemaNodeKind::Guid);
    CHECK(root.Find(u8"id")->value == u8"855ffed4-4da7-4fa0-9756-a95c6c842890");
    // A nested object carries its own fields.
    const SchemaNode* inner = root.Find(u8"inner");
    REQUIRE(inner != nullptr);
    CHECK(inner->kind == SchemaNodeKind::Object);
    REQUIRE(inner->children.Size() == 2u);
    CHECK(inner->At(0)->key == u8"depth");
    CHECK(inner->At(0)->value == u8"3");
    CHECK(inner->At(1)->key == u8"lit");
    CHECK(inner->At(1)->value == u8"false");
    // An array records its count and every element written, unkeyed.
    const SchemaNode* weights = root.Find(u8"weights");
    REQUIRE(weights != nullptr);
    CHECK(weights->kind == SchemaNodeKind::Array);
    CHECK(weights->count == 2u);
    REQUIRE(weights->children.Size() == 2u);
    CHECK(weights->At(0)->key.IsEmpty());
    CHECK(weights->At(0)->scalar == ScalarKind::Float32);
    CHECK(weights->At(1)->value == u8"0.75");
    const SchemaNode* items = root.Find(u8"items");
    REQUIRE(items != nullptr);
    CHECK(items->count == 2u);
    REQUIRE(items->children.Size() == 2u);
    CHECK(items->At(1)->kind == SchemaNodeKind::Object);
    CHECK(items->At(1)->Find(u8"depth")->value == u8"3");
    // A blob records its size only.
    CHECK(root.Find(u8"bytes")->kind == SchemaNodeKind::Blob);
    CHECK(root.Find(u8"bytes")->blobSize == 5u);
    CHECK(root.Find(u8"nobody") == nullptr);
    CHECK(root.At(11) == nullptr);
    // Nothing was versioned.
    CHECK(ar.VersionChain().IsEmpty());
}

TEST_CASE("schema-recorder: a versioned payload's chain is recorded as pushed, and the fields follow the "
          "dataVersions array in the recording")
{
    SchemaRecorder ar(DefaultAllocator());
    BeginVersionedPayload(ar, TypeOf<Inner>());
    Inner inner;
    inner.depth = 9;
    foundation::core::Serialize(ar, "depth", inner.depth);
    EndVersionedPayload(ar);
    CHECK(ar.IsOk());

    // The chain: the concrete type's id and current data version (bases only when versioned).
    REQUIRE(ar.VersionChain().Size() == 1u);
    CHECK(ar.VersionChain()[0].typeId == TypeOf<Inner>().id);
    CHECK(ar.VersionChain()[0].version == TypeOf<Inner>().dataVersion);
    // And the recording shows what the wire carries first: the dataVersions array of
    // {type, version} pairs, then the body's fields.
    const SchemaNode& root = ar.Root();
    REQUIRE(root.children.Size() == 2u);
    CHECK(root.At(0)->key == u8"dataVersions");
    CHECK(root.At(0)->kind == SchemaNodeKind::Array);
    CHECK(root.At(0)->count == 1u);
    REQUIRE(root.At(0)->children.Size() == 2u); // type, version: written flat per entry
    CHECK(root.At(0)->At(0)->key == u8"type");
    CHECK(root.At(0)->At(0)->scalar == ScalarKind::UInt64);
    CHECK(root.At(0)->At(0)->value == Format(u8"{}", TypeOf<Inner>().id));
    CHECK(root.At(0)->At(1)->key == u8"version");
    CHECK(root.At(1)->key == u8"depth");
    CHECK(root.At(1)->value == u8"9");

    // A second, inner versioned payload does not replace the outermost chain.
    SchemaRecorder nested(DefaultAllocator());
    const SerializedDataVersion outer[] = {{111u, 3u}, {222u, 5u}};
    nested.PushVersionScope(outer, 2);
    const SerializedDataVersion innerChain[] = {{333u, 1u}};
    nested.PushVersionScope(innerChain, 1);
    nested.PopVersionScope();
    nested.PopVersionScope();
    REQUIRE(nested.VersionChain().Size() == 2u);
    CHECK(nested.VersionChain()[1].typeId == 222u);
}

TEST_CASE("schema-recorder: ScalarText spells every kind the way a default reads")
{
    const bool t = true;
    const i8 i8v = -3;
    const u8 u8v = 200;
    const i16 i16v = -300;
    const u16 u16v = 60000;
    const i32 i32v = -70000;
    const u32 u32v = 4000000000u;
    const i64 i64v = -5000000000LL;
    const u64 u64v = 18000000000000000000ULL;
    const f32 f32v = 0.5f;
    const f64 f64v = -2.25;
    CHECK(SchemaRecorder::ScalarText(&t, ScalarKind::Bool) == u8"true");
    CHECK(SchemaRecorder::ScalarText(&i8v, ScalarKind::Int8) == u8"-3");
    CHECK(SchemaRecorder::ScalarText(&u8v, ScalarKind::UInt8) == u8"200");
    CHECK(SchemaRecorder::ScalarText(&i16v, ScalarKind::Int16) == u8"-300");
    CHECK(SchemaRecorder::ScalarText(&u16v, ScalarKind::UInt16) == u8"60000");
    CHECK(SchemaRecorder::ScalarText(&i32v, ScalarKind::Int32) == u8"-70000");
    CHECK(SchemaRecorder::ScalarText(&u32v, ScalarKind::UInt32) == u8"4000000000");
    CHECK(SchemaRecorder::ScalarText(&i64v, ScalarKind::Int64) == u8"-5000000000");
    CHECK(SchemaRecorder::ScalarText(&u64v, ScalarKind::UInt64) == u8"18000000000000000000");
    CHECK(SchemaRecorder::ScalarText(&f32v, ScalarKind::Float32) == u8"0.5");
    const f32 tenth = 0.1f; // not 0.10000000149011612: a default reads as authored
    CHECK(SchemaRecorder::ScalarText(&tenth, ScalarKind::Float32) == u8"0.1");
    CHECK(SchemaRecorder::ScalarText(&f64v, ScalarKind::Float64) == u8"-2.25");
}
