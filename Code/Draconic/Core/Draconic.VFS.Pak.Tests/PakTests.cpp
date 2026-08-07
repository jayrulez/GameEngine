#include <doctest/doctest.h>

#include "Draconic.Foundation/Prelude.h"
#include <cstring>

import draconic.foundation;
import draconic.vfs;
import draconic.vfs.pak;

using namespace draconic::foundation;
using namespace draconic::vfs;

namespace
{
    Span<const byte> Bytes(const char* s)
    {
        return Span<const byte>{reinterpret_cast<const byte*>(s), std::strlen(s)};
    }
}

TEST_CASE("vfs.pak: build, open, read, enumerate")
{
    const StringView pak = u8"draconic_pak_test.pak";
    FileDelete(pak);

    // --- build ---
    {
        PakBuilder builder;
        builder.Add(u8"hello.txt", Bytes("hello world"));
        builder.Add(u8"data/blob.bin", Bytes("XYZ"));
        builder.Add(u8"data/deep/leaf.txt", Bytes("leaf"));
        REQUIRE(builder.Write(pak).IsOk());
    }

    // --- open ---
    PakFileSystem fs(pak);
    REQUIRE(fs.IsValid());
    CHECK(fs.EntryCount() == 3u);

    // Capabilities: read + enumerate, but not writable.
    CHECK(fs.AsEnumerable() != nullptr);
    CHECK(fs.AsWritable() == nullptr);

    // Existence.
    CHECK(fs.Exists(u8"hello.txt"));
    CHECK(fs.Exists(u8"data/blob.bin"));
    CHECK_FALSE(fs.Exists(u8"missing.txt"));

    // Read an entry back.
    {
        UniquePtr<IStream> stream = fs.Open(u8"hello.txt", FileMode::Read);
        REQUIRE(static_cast<bool>(stream));
        char buffer[16] = {};
        const u64 n = stream->Read(buffer, 11);
        CHECK(n == 11u);
        CHECK(std::strncmp(buffer, "hello world", 11) == 0);
    }

    // Write mode is unsupported (read-only archive).
    CHECK_FALSE(static_cast<bool>(fs.Open(u8"hello.txt", FileMode::Write)));
    // Missing entry.
    CHECK_FALSE(static_cast<bool>(fs.Open(u8"nope", FileMode::Read)));

    // Enumerate root: a file (hello.txt) and a directory (data/).
    {
        Array<DirEntry> entries;
        REQUIRE(fs.Enumerate(u8"", entries).IsOk());
        bool foundFile = false;
        bool foundDir = false;
        for (const DirEntry& e : entries)
        {
            if (e.name == u8"hello.txt")
            {
                foundFile = true;
                CHECK_FALSE(e.isDirectory);
            }
            if (e.name == u8"data")
            {
                foundDir = true;
                CHECK(e.isDirectory);
            }
        }
        CHECK(foundFile);
        CHECK(foundDir);
    }

    // Enumerate a subfolder: a file (blob.bin) and a nested directory (deep/).
    {
        Array<DirEntry> entries;
        REQUIRE(fs.Enumerate(u8"data", entries).IsOk());
        bool foundBlob = false;
        bool foundDeep = false;
        for (const DirEntry& e : entries)
        {
            if (e.name == u8"blob.bin")
            {
                foundBlob = true;
                CHECK_FALSE(e.isDirectory);
            }
            if (e.name == u8"deep")
            {
                foundDeep = true;
                CHECK(e.isDirectory);
            }
        }
        CHECK(foundBlob);
        CHECK(foundDeep);
    }

    FileDelete(pak);
}

TEST_CASE("vfs.pak: a non-pak file is rejected")
{
    const StringView path = u8"draconic_pak_bad.pak";
    const byte junk[] = {byte{1}, byte{2}, byte{3}, byte{4}};
    REQUIRE(WriteFile(path, Span<const byte>{junk, ArrayCount(junk)}).IsOk());

    PakFileSystem fs(path);
    CHECK_FALSE(fs.IsValid());
    CHECK_FALSE(fs.Exists(u8"anything"));

    FileDelete(path);
}
