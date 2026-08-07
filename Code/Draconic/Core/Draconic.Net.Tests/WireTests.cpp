// draconic.net:wire - bit-level pack/unpack, varints, ranged-float quantization, overflow safety.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"

import draconic.foundation;
import draconic.net;

using namespace draconic::foundation;
namespace net = draconic::net;

TEST_CASE("wire: arbitrary bit widths round-trip in order")
{
    net::BitWriter w;
    w.WriteBits(1u, 1);
    w.WriteBits(5u, 3); // 101
    w.WriteBits(0u, 4);
    w.WriteBits(0x2AAu, 10); // spans a byte boundary
    w.WriteBits(0xFFFFFFFFu, 32);

    net::BitReader r(w.Data());
    CHECK(r.ReadBits(1) == 1u);
    CHECK(r.ReadBits(3) == 5u);
    CHECK(r.ReadBits(4) == 0u);
    CHECK(r.ReadBits(10) == 0x2AAu);
    CHECK(r.ReadBits(32) == 0xFFFFFFFFu);
    CHECK(r.Ok());
    // 50 bits written => padded to 7 bytes, so 6 padding bits remain (AtEnd is byte-granular).
}

TEST_CASE("wire: typed helpers round-trip (bool/int/float/u64)")
{
    net::BitWriter w;
    w.WriteBool(true);
    w.WriteBool(false);
    w.WriteU8(0xABu);
    w.WriteU16(0x1234u);
    w.WriteU32(0xDEADBEEFu);
    w.WriteU64(0x1122334455667788ull);
    w.WriteI32(-42);
    w.WriteFloat(3.14159f);
    w.WriteFloat(-0.0f);

    net::BitReader r(w.Data());
    CHECK(r.ReadBool() == true);
    CHECK(r.ReadBool() == false);
    CHECK(r.ReadU8() == 0xABu);
    CHECK(r.ReadU16() == 0x1234u);
    CHECK(r.ReadU32() == 0xDEADBEEFu);
    CHECK(r.ReadU64() == 0x1122334455667788ull);
    CHECK(r.ReadI32() == -42);
    CHECK(r.ReadFloat() == doctest::Approx(3.14159f));
    CHECK(r.ReadFloat() == 0.0f);
    CHECK(r.Ok());
}

TEST_CASE("wire: varint packs small values in one byte and round-trips large ones")
{
    net::BitWriter w;
    w.WriteVarU32(0u);
    w.WriteVarU32(127u); // 1 byte
    w.WriteVarU32(128u); // 2 bytes
    w.WriteVarU32(300u);
    w.WriteVarU32(0xFFFFFFFFu); // 5 bytes
    // 0(1) + 127(1) + 128(2) + 300(2) + max(5) = 11 bytes
    CHECK(w.ByteCount() == 11u);

    net::BitReader r(w.Data());
    CHECK(r.ReadVarU32() == 0u);
    CHECK(r.ReadVarU32() == 127u);
    CHECK(r.ReadVarU32() == 128u);
    CHECK(r.ReadVarU32() == 300u);
    CHECK(r.ReadVarU32() == 0xFFFFFFFFu);
    CHECK(r.Ok());
}

TEST_CASE("wire: ranged-float quantization stays within the bit-width's resolution")
{
    net::BitWriter w;
    w.WriteFloatRanged(0.0f, -1.0f, 1.0f, 16);
    w.WriteFloatRanged(0.5f, 0.0f, 1.0f, 16);
    w.WriteFloatRanged(2.0f, 0.0f, 1.0f, 8);  // clamps to max
    w.WriteFloatRanged(-9.0f, 0.0f, 1.0f, 8); // clamps to min

    net::BitReader r(w.Data());
    CHECK(r.ReadFloatRanged(-1.0f, 1.0f, 16) == doctest::Approx(0.0f).epsilon(0.001));
    CHECK(r.ReadFloatRanged(0.0f, 1.0f, 16) == doctest::Approx(0.5f).epsilon(0.001));
    CHECK(r.ReadFloatRanged(0.0f, 1.0f, 8) == doctest::Approx(1.0f));
    CHECK(r.ReadFloatRanged(0.0f, 1.0f, 8) == doctest::Approx(0.0f));
    CHECK(r.Ok());
}

TEST_CASE("wire: WriteBytes/ReadBytes round-trip a payload")
{
    const u8 payload[] = {0x00, 0x7F, 0x80, 0xFF, 0x42, 0x13};
    net::BitWriter w;
    w.WriteBool(true); // force a non-byte-aligned start
    w.WriteBytes(Span<const byte>(reinterpret_cast<const byte*>(payload), sizeof(payload)));

    net::BitReader r(w.Data());
    CHECK(r.ReadBool() == true);
    byte out[sizeof(payload)] = {};
    r.ReadBytes(Span<byte>(out, sizeof(out)));
    for (usize i = 0; i < sizeof(payload); ++i)
    {
        CHECK(static_cast<u8>(out[i]) == payload[i]);
    }
    CHECK(r.Ok());
}

TEST_CASE("wire: reading past the end degrades safely (Ok() == false)")
{
    net::BitWriter w;
    w.WriteU16(0xBEEFu);

    net::BitReader r(w.Data());
    CHECK(r.ReadU16() == 0xBEEFu);
    CHECK(r.Ok());
    CHECK(r.AtEnd());
    (void)r.ReadU32();   // past the end
    CHECK_FALSE(r.Ok()); // overflow flagged, not garbage read
}
