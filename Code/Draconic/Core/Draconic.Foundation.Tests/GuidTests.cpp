#include <doctest/doctest.h>

#include "Draconic.Foundation/Prelude.h" // brings <new> into reach for container instantiation (GCC)

import draconic.foundation;

using namespace draconic::foundation;

TEST_CASE("guid: nil and default")
{
    CHECK(Guid{}.IsNil());
    CHECK(Guid::Nil.IsNil());
    CHECK(!static_cast<bool>(Guid::Nil));

    const Guid g{1, 2};
    CHECK(!g.IsNil());
    CHECK(static_cast<bool>(g));
}

TEST_CASE("guid: generation is deterministic per seed and v4-tagged")
{
    Random a(777);
    Random b(777);
    Random c(778);

    const Guid ga = Guid::Generate(a);
    const Guid gb = Guid::Generate(b);
    const Guid gc = Guid::Generate(c);

    CHECK(ga == gb); // same seed -> same id
    CHECK(ga != gc); // different seed -> different id
    CHECK(!ga.IsNil());

    // Version nibble (4) and variant bits (10xx) per RFC 4122.
    CHECK(((ga.high >> 12) & 0xF) == 0x4);
    CHECK(((ga.low >> 62) & 0x3) == 0x2);
}

TEST_CASE("guid: ToChars / TryParse round-trip")
{
    Random rng(2025);
    const Guid g = Guid::Generate(rng);

    utf8char text[37];
    g.ToChars(text);

    // Canonical layout: 8-4-4-4-12 with dashes at fixed positions.
    CHECK(text[8] == utf8char('-'));
    CHECK(text[13] == utf8char('-'));
    CHECK(text[18] == utf8char('-'));
    CHECK(text[23] == utf8char('-'));
    CHECK(text[36] == utf8char('\0'));

    Guid parsed{};
    CHECK(Guid::TryParse(StringView(text, 36), parsed));
    CHECK(parsed == g);
}

TEST_CASE("guid: TryParse rejects malformed input")
{
    Guid out{9, 9};
    CHECK(!Guid::TryParse(StringView{}, out));
    CHECK(!Guid::TryParse(u8"not-a-guid", out));
    CHECK(!Guid::TryParse(u8"00000000-0000-0000-0000-00000000000", out));  // too short
    CHECK(!Guid::TryParse(u8"00000000+0000-0000-0000-000000000000", out)); // wrong separator
    CHECK(!Guid::TryParse(u8"0000000g-0000-0000-0000-000000000000", out)); // non-hex
    CHECK(out == Guid{9, 9});                                              // unchanged on failure

    // Accepts uppercase hex.
    Guid ok{};
    CHECK(Guid::TryParse(u8"FFFFFFFF-FFFF-FFFF-FFFF-FFFFFFFFFFFF", ok));
    CHECK(ok.high == 0xFFFFFFFFFFFFFFFFull);
    CHECK(ok.low == 0xFFFFFFFFFFFFFFFFull);
}

TEST_CASE("guid: formats via {} to its canonical string")
{
    Random rng(42);
    const Guid g = Guid::Generate(rng);

    utf8char expected[37];
    g.ToChars(expected);

    FormatBuffer buffer;
    FormatTo(buffer, u8"id={}", g);

    CHECK(buffer.Size() == 3 + 36); // "id=" + 36-char guid
    CHECK(buffer.View().SubStr(0, 3) == StringView(u8"id="));

    // The format buffer is UTF-8, matching the guid's UTF-8 ToChars output.
    CHECK(buffer.View().SubStr(3, 36) == StringView(expected, 36));
}

TEST_CASE("guid: usable as a hashed-container key")
{
    Random rng(1);
    HashSet<Guid> set;
    Guid first{};
    for (int i = 0; i < 64; ++i)
    {
        const Guid g = Guid::Generate(rng);
        if (i == 0)
        {
            first = g;
        }
        set.Insert(g);
    }
    CHECK(set.Size() == 64); // all distinct
    CHECK(set.Contains(first));
    CHECK(!set.Contains(Guid::Nil));
}
