// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Tests for the XML serialization backend: round-trips through Core's
// Serialize() driver in both directions, plus a look at the emitted XML.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import foundation.xml;
import foundation.xml.serialization;
using namespace foundation::core;
using namespace foundation::xml;

namespace
{
    bool Contains(StringView h, StringView n)
    {
        if (n.Size() > h.Size())
        {
            return false;
        }
        for (usize i = 0; i + n.Size() <= h.Size(); ++i)
        {
            bool m = true;
            for (usize j = 0; j < n.Size(); ++j)
            {
                if (h[i + j] != n[j])
                {
                    m = false;
                    break;
                }
            }
            if (m)
            {
                return true;
            }
        }
        return false;
    }
}

TEST_CASE("xml.serialize: scalar + string round-trip")
{
    String out;
    {
        XmlSerializer w(foundation::core::DefaultAllocator());
        i32 a = -42;
        u32 b = 7u;
        f32 c = 1.5f;
        bool d = true;
        String s = u8"hello world";
        Serialize(w, "a", a);
        Serialize(w, "b", b);
        Serialize(w, "c", c);
        Serialize(w, "d", d);
        Serialize(w, "s", s);
        w.GetOutput(out);
        CHECK(w.IsOk());
    }

    CHECK(Contains(out, u8"name=\"a\""));
    CHECK(Contains(out, u8"<i32 name=\"a\">-42</i32>"));
    CHECK(Contains(out, u8"<string name=\"s\">hello world</string>"));

    XmlDocument doc(foundation::core::DefaultAllocator());
    REQUIRE(doc.Parse(out) == XmlResult::Ok);
    XmlSerializer r(doc);
    i32 a = 0;
    u32 b = 0;
    f32 c = 0;
    bool d = false;
    String s;
    Serialize(r, "a", a);
    Serialize(r, "b", b);
    Serialize(r, "c", c);
    Serialize(r, "d", d);
    Serialize(r, "s", s);
    CHECK(r.IsOk());
    CHECK(a == -42);
    CHECK(b == 7u);
    CHECK(c == 1.5f);
    CHECK(d == true);
    CHECK(s == StringView(u8"hello world"));
}

TEST_CASE("xml.serialize: nested object (Float3) round-trip")
{
    String out;
    {
        XmlSerializer w(foundation::core::DefaultAllocator());
        Float3 v{1.0f, 2.5f, -3.0f};
        Serialize(w, "pos", v);
        w.GetOutput(out);
    }
    CHECK(Contains(out, u8"<object name=\"pos\">"));

    XmlDocument doc(foundation::core::DefaultAllocator());
    REQUIRE(doc.Parse(out) == XmlResult::Ok);
    XmlSerializer r(doc);
    Float3 v{};
    Serialize(r, "pos", v);
    CHECK(r.IsOk());
    CHECK(v.x == 1.0f);
    CHECK(v.y == 2.5f);
    CHECK(v.z == -3.0f);
}

TEST_CASE("xml.serialize: dynamic array round-trip")
{
    String out;
    {
        XmlSerializer w(foundation::core::DefaultAllocator());
        Array<i32> nums;
        nums.PushBack(10);
        nums.PushBack(20);
        nums.PushBack(30);
        Serialize(w, "nums", nums);
        w.GetOutput(out);
    }
    CHECK(Contains(out, u8"<array name=\"nums\" count=\"3\">"));

    XmlDocument doc(foundation::core::DefaultAllocator());
    REQUIRE(doc.Parse(out) == XmlResult::Ok);
    XmlSerializer r(doc);
    Array<i32> nums;
    Serialize(r, "nums", nums);
    CHECK(r.IsOk());
    REQUIRE(nums.Size() == 3u);
    CHECK(nums[0] == 10);
    CHECK(nums[1] == 20);
    CHECK(nums[2] == 30);
}

TEST_CASE("xml.serialize: array of objects round-trip")
{
    String out;
    {
        XmlSerializer w(foundation::core::DefaultAllocator());
        Array<Float2> pts;
        pts.PushBack(Float2{1.0f, 2.0f});
        pts.PushBack(Float2{3.0f, 4.0f});
        Serialize(w, "pts", pts);
        w.GetOutput(out);
    }

    XmlDocument doc(foundation::core::DefaultAllocator());
    REQUIRE(doc.Parse(out) == XmlResult::Ok);
    XmlSerializer r(doc);
    Array<Float2> pts;
    Serialize(r, "pts", pts);
    CHECK(r.IsOk());
    REQUIRE(pts.Size() == 2u);
    CHECK(pts[0].x == 1.0f);
    CHECK(pts[0].y == 2.0f);
    CHECK(pts[1].x == 3.0f);
    CHECK(pts[1].y == 4.0f);
}

TEST_CASE("xml.serialize: Float4x4 (positional float array) round-trip")
{
    Float4x4 m = Float4x4::Identity();
    m.Data()[3] = 9.0f; // tweak one element
    String out;
    {
        XmlSerializer w(foundation::core::DefaultAllocator());
        Serialize(w, "xform", m);
        w.GetOutput(out);
    }

    XmlDocument doc(foundation::core::DefaultAllocator());
    REQUIRE(doc.Parse(out) == XmlResult::Ok);
    XmlSerializer r(doc);
    Float4x4 m2{};
    Serialize(r, "xform", m2);
    CHECK(r.IsOk());
    for (int i = 0; i < 16; ++i)
    {
        CHECK(m2.Data()[i] == m.Data()[i]);
    }
}

TEST_CASE("xml.serialize: missing field reports error on read")
{
    XmlDocument doc(foundation::core::DefaultAllocator());
    REQUIRE(doc.Parse(u8"<root><i32 name=\"a\">1</i32></root>") == XmlResult::Ok);
    XmlSerializer r(doc);
    i32 missing = 0;
    Serialize(r, "nope", missing);
    CHECK_FALSE(r.IsOk());
    CHECK(r.GetStatus().Code() == ErrorCode::NotFound);
    // And says where: what was wanted, by key, and what stood there instead.
    String place;
    r.DescribeFailure(place);
    CHECK(place == StringView(u8"no i32 'nope' at the top (next there: <i32 name=\"a\">)"));
}

TEST_CASE("xml.serialize: a failure deep inside names its path, the first failure winning")
{
    XmlDocument doc(foundation::core::DefaultAllocator());
    REQUIRE(doc.Parse(u8"<root><object name=\"outer\"><object name=\"inner\">"
                      u8"<i32 name=\"kept\">1</i32></object></object></root>") == XmlResult::Ok);
    XmlSerializer r(doc);
    r.Key("outer");
    r.BeginObject();
    r.Key("inner");
    r.BeginObject();
    f32 dropped = 0.0f;
    Serialize(r, "drag", dropped);
    Serialize(r, "later", dropped);
    String place;
    r.DescribeFailure(place);
    CHECK(place == StringView(u8"no f32 'drag' at outer/inner (next there: <i32 name=\"kept\">)"));

    // A value of the wrong kind says what it was.
    XmlDocument wrong(foundation::core::DefaultAllocator());
    REQUIRE(wrong.Parse(u8"<root><i32 name=\"n\">x</i32></root>") == XmlResult::Ok);
    XmlSerializer w(wrong);
    i32 n = 0;
    Serialize(w, "n", n);
    String what;
    w.DescribeFailure(what);
    CHECK(what == StringView(u8"not a i32: <i32> 'n' at the top (nothing left there)"));
}

namespace
{
    // A struct whose fields serialize as a flat keyed group (no per-element object wrapper),
    // the shape of a model manifest's ModelNode.
    struct FlatNode
    {
        String name;
        i32 parent = -1;
        i32 mesh = -1;
    };
    void Serialize(ISerializer& ar, FlatNode& n)
    {
        foundation::core::Serialize(ar, "name", n.name);
        foundation::core::Serialize(ar, "parent", n.parent);
        foundation::core::Serialize(ar, "mesh", n.mesh);
    }
}

TEST_CASE("xml.serialize: array of keyed structs round-trips each element distinctly")
{
    // A keyed lookup that restarts at FirstChild for every field makes every element of a flat
    // keyed-struct array read as a copy of the first one, and a model manifest then loses every
    // node's meshIndex.
    Array<FlatNode> nodes;
    for (i32 i = 0; i < 3; ++i)
    {
        FlatNode n;
        n.name = Format(u8"node{}", i);
        n.parent = i - 1;
        n.mesh = (i == 2) ? 0 : -1;
        nodes.PushBack(static_cast<FlatNode&&>(n));
    }

    String out;
    {
        XmlSerializer w(foundation::core::DefaultAllocator());
        foundation::core::Serialize(w, "nodes", nodes);
        REQUIRE(w.IsOk());
        w.GetOutput(out);
    }

    XmlDocument parsed(foundation::core::DefaultAllocator());
    REQUIRE(parsed.Parse(out.AsView()) == XmlResult::Ok);
    XmlSerializer r(parsed);
    Array<FlatNode> loaded;
    foundation::core::Serialize(r, "nodes", loaded);
    REQUIRE(r.IsOk());
    REQUIRE(loaded.Size() == 3u);
    CHECK(loaded[0].name == StringView(u8"node0"));
    CHECK(loaded[1].name == StringView(u8"node1"));
    CHECK(loaded[2].name == StringView(u8"node2"));
    CHECK(loaded[0].parent == -1);
    CHECK(loaded[1].parent == 0);
    CHECK(loaded[2].parent == 1);
    CHECK(loaded[0].mesh == -1);
    CHECK(loaded[1].mesh == -1);
    CHECK(loaded[2].mesh == 0); // not the first element's -1
}

// --- Framed regions (unknown-section passthrough). XML is self-describing, so the frame markers are
//     no-ops and RawRemainder captures and re-injects the current scope's remaining element subtree. ---

TEST_CASE("xml.serialize: BeginFramedRegion/EndFramedRegion are no-ops (identical output)")
{
    String framed;
    {
        XmlSerializer w(foundation::core::DefaultAllocator());
        w.BeginObject();
        w.BeginFramedRegion();
        w.Key("v");
        {
            u32 v = 5;
            w.Scalar(&v, ScalarKind::UInt32);
        }
        w.EndFramedRegion();
        w.EndObject();
        w.GetOutput(framed);
    }
    String unframed;
    {
        XmlSerializer w(foundation::core::DefaultAllocator());
        w.BeginObject();
        w.Key("v");
        {
            u32 v = 5;
            w.Scalar(&v, ScalarKind::UInt32);
        }
        w.EndObject();
        w.GetOutput(unframed);
    }
    CHECK(framed == unframed); // element boundaries delimit; framing adds nothing to the DOM
}

TEST_CASE("xml.serialize: RawRemainder captures an unknown payload and re-injects it")
{
    // Author a section: typeName and a framed payload object {v=42}.
    String out;
    {
        XmlSerializer w(foundation::core::DefaultAllocator());
        w.BeginObject();
        w.Key("typeName");
        {
            String s{StringView(u8"Foo")};
            w.Text(s);
        }
        w.BeginFramedRegion();
        w.Key("payload");
        w.BeginObject();
        w.Key("v");
        {
            u32 v = 42;
            w.Scalar(&v, ScalarKind::UInt32);
        }
        w.EndObject();
        w.EndFramedRegion();
        w.EndObject();
        w.GetOutput(out);
    }

    // Read as a build that does not know "Foo": capture the framed remainder as raw bytes.
    Array<u8> captured;
    {
        XmlDocument doc(foundation::core::DefaultAllocator());
        REQUIRE(doc.Parse(out) == XmlResult::Ok);
        XmlSerializer r(doc);
        r.BeginObject();
        {
            String s;
            r.Key("typeName");
            r.Text(s);
            CHECK(s.AsView() == StringView(u8"Foo"));
        }
        r.BeginFramedRegion();
        REQUIRE(r.RawRemainder(captured));
        r.EndFramedRegion();
        r.EndObject();
    }
    CHECK(captured.Size() > 0u);

    // Re-emit the captured payload into a fresh section.
    String out2;
    {
        XmlSerializer w(foundation::core::DefaultAllocator());
        w.BeginObject();
        w.Key("typeName");
        {
            String s{StringView(u8"Foo")};
            w.Text(s);
        }
        w.BeginFramedRegion();
        REQUIRE(w.RawRemainder(captured));
        w.EndFramedRegion();
        w.EndObject();
        w.GetOutput(out2);
    }

    // A build that does know "Foo" recovers v=42 from the re-emitted section.
    {
        XmlDocument doc(foundation::core::DefaultAllocator());
        REQUIRE(doc.Parse(out2) == XmlResult::Ok);
        XmlSerializer r(doc);
        r.BeginObject();
        {
            String s;
            r.Key("typeName");
            r.Text(s);
            CHECK(s.AsView() == StringView(u8"Foo"));
        }
        r.BeginFramedRegion();
        r.Key("payload");
        r.BeginObject();
        u32 v = 0;
        r.Key("v");
        r.Scalar(&v, ScalarKind::UInt32);
        r.EndObject();
        r.EndFramedRegion();
        r.EndObject();
        CHECK(v == 42u);
    }
}

namespace
{
    // A settings shape that grew: extras and scale were appended after files were saved
    // without them.
    struct GrownSettings
    {
        i32 size = 0;
        String name;
        Array<Guid> extras;
        f32 scale = 1.0f;
        void Serialize(ISerializer& ar)
        {
            foundation::core::Serialize(ar, "size", size);
            foundation::core::Serialize(ar, "name", name);
            SerializeAppended(ar, "extras", extras);
            SerializeAppended(ar, "scale", scale);
        }
    };
}

TEST_CASE("xml.serialize: an appended field reads its default from an older payload")
{
    // A payload saved before the field (no key) reads with the field at its
    // default, and one saved after it reads the value; the rest of the object either way.
    GrownSettings saved;
    saved.size = 7;
    saved.name = String(u8"level");
    Guid extra;
    REQUIRE(Guid::TryParse(u8"0f1e2d3c-4b5a-6978-8796-a5b4c3d2e1f0", extra));
    saved.extras.PushBack(extra);
    saved.scale = 2.5f;
    String text;
    {
        XmlSerializer writer(foundation::core::DefaultAllocator());
        saved.Serialize(writer);
        writer.GetOutput(text);
    }

    // Saved after: every value comes back.
    {
        XmlDocument doc(foundation::core::DefaultAllocator());
        REQUIRE(doc.Parse(text.AsView()) == XmlResult::Ok);
        XmlSerializer reader(doc);
        GrownSettings loaded;
        loaded.Serialize(reader);
        CHECK(reader.IsOk());
        CHECK(loaded.size == 7);
        CHECK(loaded.name == StringView(u8"level"));
        REQUIRE(loaded.extras.Size() == 1u);
        CHECK(loaded.extras[0] == extra);
        CHECK(loaded.scale == doctest::Approx(2.5f));
    }

    // Saved before: the appended keys were never written.
    {
        XmlDocument doc(foundation::core::DefaultAllocator());
        REQUIRE(doc.Parse(u8"<root><i32 name=\"size\">7</i32><string name=\"name\">level</string></root>") ==
                XmlResult::Ok);
        XmlSerializer reader(doc);
        GrownSettings loaded;
        loaded.Serialize(reader);
        String place;
        reader.DescribeFailure(place);
        CAPTURE(reinterpret_cast<const char*>(place.CStr()));
        CHECK(reader.IsOk());
        CHECK(loaded.size == 7); // the rest still reads
        CHECK(loaded.name == StringView(u8"level"));
        CHECK(loaded.extras.IsEmpty()); // the appended ones keep their defaults
        CHECK(loaded.scale == doctest::Approx(1.0f));
    }
}
