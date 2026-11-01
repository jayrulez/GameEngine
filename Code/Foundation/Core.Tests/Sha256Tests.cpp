// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// SHA-256 against the published test vectors (FIPS 180-4's examples, and the million 'a's), in one
// piece and fed in pieces that straddle its 64-byte blocks.
#include <doctest/doctest.h>

#include "Core/Prelude.h"
#include <initializer_list>

import foundation.core;

using namespace foundation::core;

namespace
{
    Span<const byte> Bytes(StringView text) { return Span<const byte>(reinterpret_cast<const byte*>(text.Data()), text.Size()); }
    doctest::String C(const String& s) { return doctest::String(reinterpret_cast<const char*>(s.CStr())); }
}

TEST_CASE("sha256: the published vectors")
{
    CHECK(C(Sha256::Of(Bytes(u8"")).ToHex()) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(C(Sha256::Of(Bytes(u8"abc")).ToHex()) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(C(Sha256::Of(Bytes(u8"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")).ToHex()) ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    // 56 bytes and 64 bytes: the length's padding spills into a second block, or fills one.
    CHECK(C(Sha256::Of(Bytes(u8"The quick brown fox jumps over the lazy dog")).ToHex()) ==
          "d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592");
}

TEST_CASE("sha256: fed in pieces, the same digest as at once")
{
    Sha256 million;
    String thousand;
    for (int i = 0; i < 1000; ++i)
        thousand.Append(u8'a');
    for (int i = 0; i < 1000; ++i)
        million.Update(Bytes(thousand.AsView()));
    CHECK(C(million.Finish().ToHex()) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");

    // Pieces of 1, 63 and 65 bytes across block edges.
    String text;
    for (int i = 0; i < 300; ++i)
        text.Append(static_cast<char8_t>(u8'a' + i % 26));
    const Sha256Digest whole = Sha256::Of(Bytes(text.AsView()));
    Sha256 pieces;
    usize at = 0;
    for (const usize size : {usize(1), usize(63), usize(65), usize(171)})
    {
        pieces.Update(Bytes(text.AsView().SubStr(at, size)));
        at += size;
    }
    CHECK(at == text.Size());
    CHECK(pieces.Finish() == whole);
}
