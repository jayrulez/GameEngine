// Draconic Foundation - :string_util tests: character classification and whitespace trimming.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
import draconic.foundation;

namespace foundation = draconic::foundation;

TEST_CASE("string-util: IsWhiteSpace / IsDigit / IsHexDigit")
{
    CHECK(foundation::IsWhiteSpace(u8' '));
    CHECK(foundation::IsWhiteSpace(u8'\t'));
    CHECK(foundation::IsWhiteSpace(u8'\n'));
    CHECK_FALSE(foundation::IsWhiteSpace(u8'x'));

    CHECK(foundation::IsDigit(u8'0'));
    CHECK(foundation::IsDigit(u8'9'));
    CHECK_FALSE(foundation::IsDigit(u8'a'));

    CHECK(foundation::IsHexDigit(u8'0'));
    CHECK(foundation::IsHexDigit(u8'f'));
    CHECK(foundation::IsHexDigit(u8'F'));
    CHECK_FALSE(foundation::IsHexDigit(u8'g'));
}

TEST_CASE("string-util: HexValue")
{
    CHECK(foundation::HexValue(u8'0') == 0);
    CHECK(foundation::HexValue(u8'9') == 9);
    CHECK(foundation::HexValue(u8'a') == 10);
    CHECK(foundation::HexValue(u8'F') == 15);
    CHECK(foundation::HexValue(u8'z') == -1);
}

TEST_CASE("string-util: Trim / TrimStart / TrimEnd")
{
    CHECK(foundation::Trim(foundation::StringView(u8"  hi  ")) == foundation::StringView(u8"hi"));
    CHECK(foundation::Trim(foundation::StringView(u8"\t\n x \r")) == foundation::StringView(u8"x"));
    CHECK(foundation::Trim(foundation::StringView(u8"nospace")) == foundation::StringView(u8"nospace"));
    CHECK(foundation::Trim(foundation::StringView(u8"   ")).Size() == 0); // all whitespace -> empty
    CHECK(foundation::Trim(foundation::StringView(u8"")).Size() == 0);

    CHECK(foundation::TrimStart(foundation::StringView(u8"  hi  ")) == foundation::StringView(u8"hi  "));
    CHECK(foundation::TrimEnd(foundation::StringView(u8"  hi  ")) == foundation::StringView(u8"  hi"));

    // Internal whitespace is preserved.
    CHECK(foundation::Trim(foundation::StringView(u8"  a b c  ")) == foundation::StringView(u8"a b c"));
}

TEST_CASE("string-util: UTF-8 codepoint boundaries")
{
    // "aé€b": a=1 byte, é=2 bytes (0xC3 0xA9), €=3 bytes (0xE2 0x82 0xAC), b=1 byte. 7 bytes.
    const foundation::StringView s(u8"aé€b");
    CHECK(s.Size() == 7);

    // Continuation-byte classification.
    CHECK_FALSE(foundation::IsUtf8Continuation(s[0])); // 'a'
    CHECK_FALSE(foundation::IsUtf8Continuation(s[1])); // é lead
    CHECK(foundation::IsUtf8Continuation(s[2]));       // é trail
    CHECK_FALSE(foundation::IsUtf8Continuation(s[3])); // € lead
    CHECK(foundation::IsUtf8Continuation(s[4]));       // € trail 1
    CHECK(foundation::IsUtf8Continuation(s[5]));       // € trail 2
    CHECK_FALSE(foundation::IsUtf8Continuation(s[6])); // 'b'

    // Forward: 0 -> 1 (a) -> 3 (é) -> 6 (€) -> 7 (b) -> 7 (clamped).
    CHECK(foundation::Utf8NextBoundary(s, 0) == 1);
    CHECK(foundation::Utf8NextBoundary(s, 1) == 3);
    CHECK(foundation::Utf8NextBoundary(s, 3) == 6);
    CHECK(foundation::Utf8NextBoundary(s, 6) == 7);
    CHECK(foundation::Utf8NextBoundary(s, 7) == 7);

    // Backward: 7 -> 6 -> 3 -> 1 -> 0 -> 0 (clamped).
    CHECK(foundation::Utf8PrevBoundary(s, 7) == 6);
    CHECK(foundation::Utf8PrevBoundary(s, 6) == 3);
    CHECK(foundation::Utf8PrevBoundary(s, 3) == 1);
    CHECK(foundation::Utf8PrevBoundary(s, 1) == 0);
    CHECK(foundation::Utf8PrevBoundary(s, 0) == 0);

    // Prev from an interior byte snaps to the codepoint start it is inside/after.
    CHECK(foundation::Utf8PrevBoundary(s, 5) == 3); // inside €
}
