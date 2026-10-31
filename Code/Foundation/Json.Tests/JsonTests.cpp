// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Unit tests for the JSON DOM (parse, write, build): foundation.json in isolation, no reflection
// or scripting. The RTTI/script-usability flow is a cross-collection integration test and lives in
// Code/Integration/Integration.Script.
#include <doctest/doctest.h>

#include <initializer_list>

#include "Core/Prelude.h"

import foundation.core;
import foundation.json;

using namespace foundation::core;
using namespace foundation::json;

// --- Parse: scalars --------------------------------------------------------

TEST_CASE("json.parse: scalars")
{
    CHECK(Parse(u8"null").ok);
    CHECK(Parse(u8"null").value.IsNull());

    ParseResult t = Parse(u8"true");
    CHECK(t.ok);
    CHECK(t.value.IsBool());
    CHECK(t.value.AsBool() == true);

    CHECK(Parse(u8"false").value.AsBool() == false);

    ParseResult n = Parse(u8"  -12.5e2 ");
    CHECK(n.ok);
    CHECK(n.value.IsNumber());
    CHECK(n.value.AsNumber() == doctest::Approx(-1250.0));

    ParseResult i = Parse(u8"42");
    CHECK(i.value.AsInt() == 42);

    ParseResult s = Parse(u8"\"hi\"");
    CHECK(s.ok);
    CHECK(s.value.IsString());
    CHECK(s.value.AsString() == StringView(u8"hi"));
}

// --- Parse: strings and escapes --------------------------------------------

TEST_CASE("json.parse: string escapes")
{
    CHECK(Parse(u8"\"a\\nb\"").value.AsString() == StringView(u8"a\nb"));
    CHECK(Parse(u8"\"tab\\tend\"").value.AsString() == StringView(u8"tab\tend"));
    CHECK(Parse(u8"\"q\\\"q\"").value.AsString() == StringView(u8"q\"q"));
    CHECK(Parse(u8"\"sl\\/sl\"").value.AsString() == StringView(u8"sl/sl"));

    // \u00XX control + BMP codepoint (U+00E9 e-acute -> UTF-8 C3 A9).
    ParseResult u = Parse(u8"\"\\u00e9\"");
    CHECK(u.ok);
    const String sv = u.value.AsString(); // hold the owned copy (AsString returns String, not a view)
    REQUIRE(sv.Size() == 2u);
    CHECK(static_cast<unsigned char>(sv.Data()[0]) == 0xC3u);
    CHECK(static_cast<unsigned char>(sv.Data()[1]) == 0xA9u);

    // Surrogate pair for U+1F600 (grinning face) -> 4-byte UTF-8 F0 9F 98 80.
    ParseResult e = Parse(u8"\"\\ud83d\\ude00\"");
    CHECK(e.ok);
    CHECK(e.value.AsString().Size() == 4u);
}

// --- Parse: containers -----------------------------------------------------

TEST_CASE("json.parse: arrays and objects")
{
    ParseResult a = Parse(u8"[1, 2, [3, 4], null]");
    CHECK(a.ok);
    CHECK(a.value.IsArray());
    CHECK(a.value.Count() == 4);
    CHECK(a.value.At(0).AsNumber() == doctest::Approx(1.0));
    CHECK(a.value.At(2).IsArray());
    CHECK(a.value.At(2).At(1).AsInt() == 4);
    CHECK(a.value.At(3).IsNull());
    CHECK(a.value.At(99).IsNull()); // out of range -> null copy, no crash

    ParseResult o = Parse(u8"{\"name\":\"raptor\",\"n\":3,\"nested\":{\"ok\":true}}");
    CHECK(o.ok);
    CHECK(o.value.IsObject());
    CHECK(o.value.Count() == 3);
    CHECK(o.value.Has(u8"name"));
    CHECK(o.value.Get(u8"name").AsString() == StringView(u8"raptor"));
    CHECK(o.value.Get(u8"n").AsInt() == 3);
    CHECK(o.value.Get(u8"nested").Get(u8"ok").AsBool() == true);
    CHECK(o.value.Get(u8"missing").IsNull()); // absent -> null copy
    CHECK(o.value.KeyAt(0) == StringView(u8"name"));

    CHECK(Parse(u8"[]").value.Count() == 0);
    CHECK(Parse(u8"{}").value.IsObject());
    CHECK(Parse(u8"{}").value.Count() == 0);
}

// --- Parse: errors ---------------------------------------------------------

TEST_CASE("json.parse: errors return a clean null + position")
{
    for (const char8_t* bad : {u8"", u8"{", u8"[1,]", u8"{\"a\":}", u8"nul", u8"\"open",
                               u8"[1 2]", u8"{a:1}", u8"1 2", u8"\"bad\\x\""})
    {
        ParseResult r = Parse(bad);
        CHECK_FALSE(r.ok);
        CHECK(r.value.IsNull());   // never a half-value
        CHECK(r.error.Size() > 0u);
    }
}

TEST_CASE("json.parse: a leading zero is not a number (RFC 8259)")
{
    CHECK_FALSE(Parse(u8"01").ok);
    CHECK_FALSE(Parse(u8"-01").ok);
    CHECK_FALSE(Parse(u8"[1, 007]").ok);
    CHECK_FALSE(Parse(u8"{\"a\": 00}").ok);
    // The legitimate zero forms still parse.
    CHECK(Parse(u8"0").ok);
    CHECK(Parse(u8"-0").ok);
    CHECK(Parse(u8"0.5").value.AsNumber() == doctest::Approx(0.5));
    CHECK(Parse(u8"0e2").ok);
    CHECK(Parse(u8"[0, 10, 100]").ok);
}

TEST_CASE("json.parse: nesting-depth guard rejects hostile input")
{
    String deep;
    for (i32 i = 0; i < 5000; ++i)
    {
        deep.PushBack(u8'[');
    }
    ParseResult r = Parse(deep.AsView());
    CHECK_FALSE(r.ok); // guarded, not a stack overflow
}

// --- Write and round-trip --------------------------------------------------

TEST_CASE("json.write: compact + round-trip")
{
    // Numbers print shortest (42 not 42.0); order preserved for objects.
    CHECK(JsonValue::MakeNumber(42).ToString() == StringView(u8"42"));
    CHECK(JsonValue::MakeNull().ToString() == StringView(u8"null"));
    CHECK(JsonValue::MakeBool(true).ToString() == StringView(u8"true"));
    CHECK(JsonValue::MakeString(u8"a\"b\n").ToString() == StringView(u8"\"a\\\"b\\n\""));

    const StringView src = u8"{\"a\":1,\"b\":[true,null,\"x\"],\"c\":{}}";
    // The reflected static JsonValue::Parse (the script-facing entry, address-taken by the
    // reflection registration) parses identically to the free function.
    CHECK(JsonValue::Parse(String(src)).ToString() == src);
    ParseResult r = Parse(src);
    REQUIRE(r.ok);
    CHECK(r.value.ToString() == src); // compact write is byte-stable for this input

    // Re-parse the pretty form yields the same tree.
    const String pretty = r.value.ToString(true);
    CHECK(pretty.Size() > src.Size());
    ParseResult back = Parse(pretty.AsView());
    REQUIRE(back.ok);
    CHECK(back.value.ToString() == src);
}

// --- Build via the API (the script surface, exercised from C++) ------------

TEST_CASE("json.build: construct + mutate is value-semantic")
{
    JsonValue obj = JsonValue::MakeObject();
    obj.Set(u8"id", JsonValue::MakeNumber(7));
    obj.Set(u8"name", JsonValue::MakeString(u8"tool"));

    JsonValue arr = JsonValue::MakeArray();
    arr.Add(JsonValue::MakeNumber(1));
    arr.Add(JsonValue::MakeString(u8"two"));
    obj.Set(u8"args", Move(arr));

    // Overwrite keeps order, replaces value.
    obj.Set(u8"id", JsonValue::MakeNumber(8));

    CHECK(obj.Get(u8"id").AsInt() == 8);
    CHECK(obj.Get(u8"args").Count() == 2);
    CHECK(obj.ToString() == StringView(u8"{\"id\":8,\"name\":\"tool\",\"args\":[1,\"two\"]}"));

    // Get returns an owned copy: mutating it must not touch the parent (no interior aliasing).
    JsonValue got = obj.Get(u8"args");
    got.Add(JsonValue::MakeNumber(3));
    CHECK(got.Count() == 3);
    CHECK(obj.Get(u8"args").Count() == 2); // parent unchanged

    // A default-constructed value coerces on first Set/Add.
    JsonValue fresh;
    CHECK(fresh.IsNull());
    fresh.Set(u8"k", JsonValue::MakeBool(true));
    CHECK(fresh.IsObject());
}
