#include <doctest/doctest.h>

import draconic.foundation;

using namespace draconic::foundation;

// String / StringView / StringBuilder and UTF-8<->UTF-16 transcoding.
// String is the primary UTF-8 type; WideString is the UTF-16 (Win32-edge) type.

TEST_CASE("string: StringView basics")
{
    StringView v = u8"hello";
    CHECK(v.Size() == 5u);
    CHECK_FALSE(v.IsEmpty());
    CHECK(v[0] == u'h');
    CHECK(v == StringView(u8"hello"));
    CHECK_FALSE(v == StringView(u8"world"));

    CHECK(v.StartsWith(u8"he"));
    CHECK(v.EndsWith(u8"lo"));
    CHECK(v.SubStr(1, 3) == StringView(u8"ell"));

    static_assert(CStringLength(u8"abc") == 3u);
}

TEST_CASE("string: construct, append, compare")
{
    String s = u8"foo";
    CHECK(s.Size() == 3u);
    CHECK(s == u8"foo");

    s += u8"bar";
    CHECK(s == u8"foobar");
    CHECK(s.Size() == 6u);

    s.PushBack(u'!');
    CHECK(s == u8"foobar!");

    // CStr is null-terminated.
    CHECK(s.CStr()[s.Size()] == u'\0');

    String empty;
    CHECK(empty.IsEmpty());
    CHECK(empty.CStr()[0] == u'\0'); // valid even with no allocation
}

TEST_CASE("string: copy and move")
{
    String a = u8"original";
    String b = a; // deep copy
    CHECK(a == b);

    b += u8"-modified";
    CHECK_FALSE(a == b);
    CHECK(a == u8"original");

    String c = Move(a); // steal buffer
    CHECK(c == u8"original");
    CHECK(a.IsEmpty());
}

TEST_CASE("string: growth across reallocations")
{
    String s;
    for (int i = 0; i < 1000; ++i)
    {
        s.PushBack(u'x');
    }
    CHECK(s.Size() == 1000u);
    CHECK(s.Capacity() >= 1000u);
    CHECK(s[0] == u'x');
    CHECK(s[999] == u'x');
    CHECK(s.CStr()[1000] == u'\0');
}

TEST_CASE("string: String is the primary UTF-8 type")
{
    String s = u8"utf8";
    CHECK(s.Size() == 4u);
    s += u8"-data";
    CHECK(s == u8"utf8-data");

    // utf8char is 1 byte; the wide (Win32-edge) code unit is 2.
    CHECK(sizeof(String::ValueType) == 1u);
    CHECK(sizeof(WideString::ValueType) == 2u);
}

TEST_CASE("string: honours a custom allocator")
{
    alignas(64) byte buffer[2048];
    LinearAllocator arena(buffer, sizeof(buffer));

    // Long enough to spill past the small-string buffer and hit the arena.
    String s(arena);
    for (int i = 0; i < 200; ++i)
    {
        s.PushBack(u8'a');
    }
    CHECK(s.Size() == 200u);
    CHECK_FALSE(s.IsSmall());
    CHECK(arena.Used() > 0u);
}

TEST_CASE("stringbuilder: builds strings with numbers")
{
    StringBuilder sb;
    sb.Append(u8"x=").AppendInt(-42).Append(u8", ok=").AppendBool(true);
    CHECK(sb.View() == u8"x=-42, ok=true");

    sb.Clear();
    CHECK(sb.Size() == 0u);

    sb.AppendAscii("count:").Append(u8' ').AppendUInt(1000u);
    CHECK(sb.Str() == u8"count: 1000");

    String taken = sb.Take();
    CHECK(taken == u8"count: 1000");
    CHECK(sb.Size() == 0u); // moved out
}

TEST_CASE("string: UTF-8 <-> UTF-16 transcoding")
{
    // ASCII.
    CHECK(ToWide(u8"hello") == WideStringView(u"hello"));
    CHECK(ToUTF8(u"hello") == StringView(u8"hello"));

    // U+00E9 (é): 2-byte UTF-8, single UTF-16 unit.
    CHECK(ToWide(u8"café") == WideStringView(u"café"));
    CHECK(ToUTF8(u"café") == StringView(u8"café"));

    // U+1F600 (emoji): 4-byte UTF-8, surrogate pair in UTF-16.
    String emoji8 = u8"\U0001F600";
    WideString emoji16 = ToWide(emoji8);
    CHECK(emoji16.Size() == 2u); // surrogate pair
    CHECK(ToUTF8(emoji16) == emoji8);

    // Mixed round-trip both directions.
    String mixed8 = u8"aé\U0001F600z";
    CHECK(ToUTF8(ToWide(mixed8)) == mixed8);

    WideString mixed16 = u"aéz";
    CHECK(ToWide(ToUTF8(mixed16)) == mixed16);
}

TEST_CASE("string: small-string optimization avoids heap until it grows")
{
    String small = u8"hi"; // short -> inline
    CHECK(small.IsSmall());
    CHECK(small == u8"hi");
    CHECK(small.CStr()[small.Size()] == u'\0');

    // Build up a long string -> spills to the heap, content preserved.
    String big;
    for (int i = 0; i < 100; ++i)
    {
        big.PushBack(u'x');
    }
    CHECK_FALSE(big.IsSmall());
    CHECK(big.Size() == 100u);
    CHECK(big[0] == u'x');
    CHECK(big[99] == u'x');

    // Move of a heap string transfers the buffer; move of an inline copies.
    String movedBig = Move(big);
    CHECK(movedBig.Size() == 100u);
    CHECK(big.IsEmpty());

    String movedSmall = Move(small);
    CHECK(movedSmall == u8"hi");
    CHECK(small.IsEmpty());

    // Copy is independent across the inline/heap boundary.
    String copy = movedBig;
    CHECK(copy == movedBig);
    copy.PushBack(u'!');
    CHECK_FALSE(copy == movedBig);
}

// --- Insert / Remove (byte/code-unit level) --------------------------------

TEST_CASE("string: Insert opens a gap at the given index")
{
    String s = u8"Helo";
    s.Insert(2, u8"l"); // -> "Hello"
    CHECK(s == u8"Hello");

    s.Insert(0, u8">> "); // prepend
    CHECK(s == u8">> Hello");

    s.Insert(s.Size(), u8"!"); // append at end
    CHECK(s == u8">> Hello!");

    // Index past the end clamps to the end; empty insert is a no-op.
    String t = u8"ab";
    t.Insert(100, u8"c");
    CHECK(t == u8"abc");
    t.Insert(1, u8"");
    CHECK(t == u8"abc");

    // Terminator stays intact after inserts that spill to the heap.
    String big;
    for (int i = 0; i < 40; ++i)
    {
        big.PushBack(u'x');
    }
    big.Insert(20, u8"MID");
    CHECK(big.Size() == 43u);
    CHECK(big.CStr()[big.Size()] == u'\0');
    CHECK(big[20] == u'M');
}

TEST_CASE("string: Remove closes the gap and clamps ranges")
{
    String s = u8"Hello";
    s.Remove(4, 1); // drop trailing 'o' -> "Hell"
    CHECK(s == u8"Hell");

    s.Remove(0, 1); // drop leading 'H' -> "ell"
    CHECK(s == u8"ell");

    // count past the end is clamped.
    String t = u8"abcdef";
    t.Remove(3, 100);
    CHECK(t == u8"abc");

    // index >= size or count 0 is a no-op.
    String u = u8"abc";
    u.Remove(3, 1);
    CHECK(u == u8"abc");
    u.Remove(0, 0);
    CHECK(u == u8"abc");
    CHECK(u.CStr()[u.Size()] == u'\0');
}

TEST_CASE("string: Replace swaps every matching code unit in place")
{
    String s = u8"C:\\a\\b\\c";
    const usize n = s.Replace(u8'\\', u8'/');
    CHECK(n == 3u);
    CHECK(s == u8"C:/a/b/c");
    CHECK(s.Size() == 8u);              // size-preserving
    CHECK(s.CStr()[s.Size()] == u'\0'); // still null-terminated

    // No match => zero replaced, string untouched.
    String t = u8"nothing";
    CHECK(t.Replace(u8'x', u8'y') == 0u);
    CHECK(t == u8"nothing");

    // Empty string is a no-op.
    String e;
    CHECK(e.Replace(u8'a', u8'b') == 0u);
    CHECK(e.Size() == 0u);
}

// --- UTF-8 codepoint iteration & encoding ----------------------------------

TEST_CASE("string: DecodeUtf8 walks Unicode scalars")
{
    StringView v = u8"aé\U0001F600z"; // 'a'(1) 'é'(2) emoji(4) 'z'(1) = 8 bytes, 4 codepoints
    usize i = 0;
    CHECK(DecodeUtf8(v, i) == 0x61u);
    CHECK(i == 1u);
    CHECK(DecodeUtf8(v, i) == 0xE9u);
    CHECK(i == 3u);
    CHECK(DecodeUtf8(v, i) == 0x1F600u);
    CHECK(i == 7u);
    CHECK(DecodeUtf8(v, i) == 0x7Au);
    CHECK(i == 8u);
    CHECK(i == v.Size());
}

TEST_CASE("string: AppendUtf8 encodes each scalar range")
{
    String s;
    AppendUtf8(s, 0x61u);    // 'a' 1 byte
    AppendUtf8(s, 0xE9u);    // 'é' 2 bytes
    AppendUtf8(s, 0x1F600u); // emoji 4 bytes
    AppendUtf8(s, 0x7Au);    // 'z' 1 byte
    CHECK(s == u8"aé\U0001F600z");
    CHECK(s.Size() == 8u);
}

TEST_CASE("string: Utf8Length counts codepoints not bytes")
{
    CHECK(Utf8Length(u8"") == 0u);
    CHECK(Utf8Length(u8"hello") == 5u);
    CHECK(Utf8Length(u8"aé\U0001F600z") == 4u);
}

// --- Trimmed / ParseFloat / ParseInt / FormatFixed -------------------------

TEST_CASE("string: Trimmed strips ASCII whitespace both ends")
{
    CHECK(Trimmed(u8"  hi  ") == u8"hi");
    CHECK(Trimmed(u8"\t\n x \r\n") == u8"x");
    CHECK(Trimmed(u8"none") == u8"none");
    CHECK(Trimmed(u8"   ").IsEmpty());
    CHECK(Trimmed(u8"").IsEmpty());
}

TEST_CASE("string: ParseFloat parses full numbers, rejects junk")
{
    CHECK(ParseFloat(u8"3.14").HasValue());
    CHECK(ParseFloat(u8"3.14").Value() == doctest::Approx(3.14));
    CHECK(ParseFloat(u8"  -2.5 ").Value() == doctest::Approx(-2.5)); // trims first
    CHECK(ParseFloat(u8"42").Value() == doctest::Approx(42.0));
    CHECK(!ParseFloat(u8"3.14x").HasValue()); // trailing junk
    CHECK(!ParseFloat(u8"abc").HasValue());
    CHECK(!ParseFloat(u8"").HasValue());
    CHECK(!ParseFloat(u8"   ").HasValue());
}

TEST_CASE("string: ParseInt parses full integers")
{
    CHECK(ParseInt(u8"123").Value() == 123);
    CHECK(ParseInt(u8" -7 ").Value() == -7);
    CHECK(!ParseInt(u8"1.5").HasValue()); // not an integer
    CHECK(!ParseInt(u8"x").HasValue());
}

TEST_CASE("string: FormatFixed formats with N decimals")
{
    CHECK(FormatFixed(3.14159, 2) == u8"3.14");
    CHECK(FormatFixed(3.14159, 4) == u8"3.1416");
    CHECK(FormatFixed(3.7, 0) == u8"4"); // rounds, no decimal point
    CHECK(FormatFixed(-2.5, 1) == u8"-2.5");
    CHECK(FormatFixed(0.0, 2) == u8"0.00");
}
