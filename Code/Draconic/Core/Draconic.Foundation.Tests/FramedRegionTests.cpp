// Framed-region capability on the Serializer contract: binary length-frames a self-delimiting region
// so a payload can be SKIPPED or CAPTURED without knowing its type, and RawRemainder moves the framed
// bytes verbatim. This is the mechanism the settings unknown-section passthrough builds on. (XML is
// self-describing, so its BeginFramedRegion/EndFramedRegion are no-ops - covered in the XML tests.)
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
import draconic.foundation;

using namespace draconic::foundation;

namespace
{
    void WriteU32(Serializer& ar, u32 v) { ar.Scalar(&v, ScalarKind::UInt32); }
    u32 ReadU32(Serializer& ar)
    {
        u32 v = 0;
        ar.Scalar(&v, ScalarKind::UInt32);
        return v;
    }
}

TEST_CASE("framed-region (binary): a frame is skippable by length")
{
    MemoryStream stream;
    {
        BinarySerializer ar(stream, SerializeMode::Write);
        ar.BeginFramedRegion();
        WriteU32(ar, 111);
        WriteU32(ar, 222);
        ar.EndFramedRegion();
        WriteU32(ar, 999); // trailing marker AFTER the frame
        CHECK(ar.IsOk());
    }
    REQUIRE(stream.Seek(0, SeekOrigin::Begin) == 0);
    {
        BinarySerializer ar(stream, SerializeMode::Read);
        ar.BeginFramedRegion();
        ar.EndFramedRegion();       // SKIP the frame without reading its contents
        CHECK(ReadU32(ar) == 999u); // the marker is found -> the frame length was honored
        CHECK(ar.IsOk());
    }
}

TEST_CASE("framed-region (binary): contents read back inside the frame")
{
    MemoryStream stream;
    {
        BinarySerializer ar(stream, SerializeMode::Write);
        ar.BeginFramedRegion();
        WriteU32(ar, 111);
        WriteU32(ar, 222);
        ar.EndFramedRegion();
        WriteU32(ar, 999);
    }
    REQUIRE(stream.Seek(0, SeekOrigin::Begin) == 0);
    {
        BinarySerializer ar(stream, SerializeMode::Read);
        ar.BeginFramedRegion();
        CHECK(ReadU32(ar) == 111u);
        CHECK(ReadU32(ar) == 222u);
        ar.EndFramedRegion();
        CHECK(ReadU32(ar) == 999u);
    }
}

TEST_CASE("framed-region (binary): RawRemainder captures + re-emits identically")
{
    MemoryStream original;
    {
        BinarySerializer ar(original, SerializeMode::Write);
        ar.BeginFramedRegion();
        WriteU32(ar, 0xABCD);
        WriteU32(ar, 0x1234);
        ar.EndFramedRegion();
    }

    // Capture the framed payload as raw bytes (as a build that does not know the type would).
    Array<u8> captured;
    REQUIRE(original.Seek(0, SeekOrigin::Begin) == 0);
    {
        BinarySerializer ar(original, SerializeMode::Read);
        ar.BeginFramedRegion();
        REQUIRE(ar.RawRemainder(captured));
        ar.EndFramedRegion();
    }
    CHECK(captured.Size() == 8u); // two u32s

    // Re-emit the captured bytes into a fresh stream; a knowing reader gets the values back.
    MemoryStream reemitted;
    {
        BinarySerializer ar(reemitted, SerializeMode::Write);
        ar.BeginFramedRegion();
        REQUIRE(ar.RawRemainder(captured));
        ar.EndFramedRegion();
    }
    CHECK(reemitted.Size() == original.Size()); // byte-identical envelope
    REQUIRE(reemitted.Seek(0, SeekOrigin::Begin) == 0);
    {
        BinarySerializer ar(reemitted, SerializeMode::Read);
        ar.BeginFramedRegion();
        CHECK(ReadU32(ar) == 0xABCDu);
        CHECK(ReadU32(ar) == 0x1234u);
        ar.EndFramedRegion();
    }
}

TEST_CASE("framed-region (binary): nested regions")
{
    MemoryStream stream;
    {
        BinarySerializer ar(stream, SerializeMode::Write);
        ar.BeginFramedRegion();
        WriteU32(ar, 1);
        ar.BeginFramedRegion();
        WriteU32(ar, 2);
        ar.EndFramedRegion();
        WriteU32(ar, 3);
        ar.EndFramedRegion();
        WriteU32(ar, 4);
    }
    REQUIRE(stream.Seek(0, SeekOrigin::Begin) == 0);
    {
        BinarySerializer ar(stream, SerializeMode::Read);
        ar.BeginFramedRegion();
        CHECK(ReadU32(ar) == 1u);
        ar.BeginFramedRegion();
        CHECK(ReadU32(ar) == 2u);
        ar.EndFramedRegion();
        CHECK(ReadU32(ar) == 3u);
        ar.EndFramedRegion();
        CHECK(ReadU32(ar) == 4u);
    }
    // Skipping the OUTER frame jumps the whole nested structure; the trailing 4 is still found.
    REQUIRE(stream.Seek(0, SeekOrigin::Begin) == 0);
    {
        BinarySerializer ar(stream, SerializeMode::Read);
        ar.BeginFramedRegion();
        ar.EndFramedRegion();
        CHECK(ReadU32(ar) == 4u);
    }
}

TEST_CASE("framed-region (binary): zero-length region")
{
    MemoryStream stream;
    {
        BinarySerializer ar(stream, SerializeMode::Write);
        ar.BeginFramedRegion();
        ar.EndFramedRegion();
        WriteU32(ar, 7);
    }
    REQUIRE(stream.Seek(0, SeekOrigin::Begin) == 0);
    {
        BinarySerializer ar(stream, SerializeMode::Read);
        ar.BeginFramedRegion();
        Array<u8> blob;
        CHECK(ar.RawRemainder(blob));
        CHECK(blob.Size() == 0u);
        ar.EndFramedRegion();
        CHECK(ReadU32(ar) == 7u);
    }
}

TEST_CASE("framed-region (binary): RawRemainder without an active frame is unsupported")
{
    MemoryStream stream;
    BinarySerializer ar(stream, SerializeMode::Write);
    Array<u8> blob;
    CHECK_FALSE(ar.RawRemainder(blob)); // no frame -> false (caller drops the section with a warning)
}
