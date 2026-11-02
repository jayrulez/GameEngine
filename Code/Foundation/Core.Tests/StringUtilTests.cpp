// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Core - :string_util tests: character classification and whitespace trimming; a size for a person.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;

namespace core = foundation::core;

TEST_CASE("string-util: IsWhiteSpace / IsDigit / IsHexDigit")
{
    CHECK(core::IsWhiteSpace(u8' '));
    CHECK(core::IsWhiteSpace(u8'\t'));
    CHECK(core::IsWhiteSpace(u8'\n'));
    CHECK_FALSE(core::IsWhiteSpace(u8'x'));

    CHECK(core::IsDigit(u8'0'));
    CHECK(core::IsDigit(u8'9'));
    CHECK_FALSE(core::IsDigit(u8'a'));

    CHECK(core::IsHexDigit(u8'0'));
    CHECK(core::IsHexDigit(u8'f'));
    CHECK(core::IsHexDigit(u8'F'));
    CHECK_FALSE(core::IsHexDigit(u8'g'));
}

TEST_CASE("string-util: HexValue")
{
    CHECK(core::HexValue(u8'0') == 0);
    CHECK(core::HexValue(u8'9') == 9);
    CHECK(core::HexValue(u8'a') == 10);
    CHECK(core::HexValue(u8'F') == 15);
    CHECK(core::HexValue(u8'z') == -1);
}

TEST_CASE("string-util: Trim / TrimStart / TrimEnd")
{
    CHECK(core::Trim(core::StringView(u8"  hi  ")) == core::StringView(u8"hi"));
    CHECK(core::Trim(core::StringView(u8"\t\n x \r")) == core::StringView(u8"x"));
    CHECK(core::Trim(core::StringView(u8"nospace")) == core::StringView(u8"nospace"));
    CHECK(core::Trim(core::StringView(u8"   ")).Size() == 0); // all whitespace -> empty
    CHECK(core::Trim(core::StringView(u8"")).Size() == 0);

    CHECK(core::TrimStart(core::StringView(u8"  hi  ")) == core::StringView(u8"hi  "));
    CHECK(core::TrimEnd(core::StringView(u8"  hi  ")) == core::StringView(u8"  hi"));

    // Internal whitespace is preserved.
    CHECK(core::Trim(core::StringView(u8"  a b c  ")) == core::StringView(u8"a b c"));
}

TEST_CASE("string-util: UTF-8 codepoint boundaries")
{
    // "aé€b": a=1 byte, é=2 bytes (0xC3 0xA9), €=3 bytes (0xE2 0x82 0xAC), b=1 byte. 7 bytes.
    const core::StringView s(u8"aé€b");
    CHECK(s.Size() == 7);

    // Continuation-byte classification.
    CHECK_FALSE(core::IsUtf8Continuation(s[0])); // 'a'
    CHECK_FALSE(core::IsUtf8Continuation(s[1])); // é lead
    CHECK(core::IsUtf8Continuation(s[2]));       // é trail
    CHECK_FALSE(core::IsUtf8Continuation(s[3])); // € lead
    CHECK(core::IsUtf8Continuation(s[4]));       // € trail 1
    CHECK(core::IsUtf8Continuation(s[5]));       // € trail 2
    CHECK_FALSE(core::IsUtf8Continuation(s[6])); // 'b'

    // Forward: 0 -> 1 (a) -> 3 (é) -> 6 (€) -> 7 (b) -> 7 (clamped).
    CHECK(core::Utf8NextBoundary(s, 0) == 1);
    CHECK(core::Utf8NextBoundary(s, 1) == 3);
    CHECK(core::Utf8NextBoundary(s, 3) == 6);
    CHECK(core::Utf8NextBoundary(s, 6) == 7);
    CHECK(core::Utf8NextBoundary(s, 7) == 7);

    // Backward: 7 -> 6 -> 3 -> 1 -> 0 -> 0 (clamped).
    CHECK(core::Utf8PrevBoundary(s, 7) == 6);
    CHECK(core::Utf8PrevBoundary(s, 6) == 3);
    CHECK(core::Utf8PrevBoundary(s, 3) == 1);
    CHECK(core::Utf8PrevBoundary(s, 1) == 0);
    CHECK(core::Utf8PrevBoundary(s, 0) == 0);

    // Prev from an interior byte snaps to the codepoint start it is inside/after.
    CHECK(core::Utf8PrevBoundary(s, 5) == 3); // inside €
}

TEST_CASE("format: a size for a person, in KB below a megabyte and MB to a tenth above")
{
    CHECK(core::ByteSizeText(0) == core::StringView(u8"1 KB")); // never "0 KB"
    CHECK(core::ByteSizeText(115461) == core::StringView(u8"113 KB"));
    CHECK(core::ByteSizeText(421504) == core::StringView(u8"412 KB"));
    CHECK(core::ByteSizeText(1024 * 1024) == core::StringView(u8"1.0 MB"));
    CHECK(core::ByteSizeText(7549747) == core::StringView(u8"7.2 MB"));
}
