// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Zip archives made by the system's tools (Data/Tests/Zip, from make-fixtures.sh): Info-ZIP's zip
// with deflated, stored and empty files and folders; one stored outright; Python's zipfile for a
// name outside the folder, a damaged entry and a comment. Read, checked and extracted; also the
// CRC-32 against its known values and which entry names are safe.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.zip;

using namespace foundation::core;
using namespace foundation::zip;

namespace
{
    String Fixture(StringView name)
    {
        return PathJoin(PathJoin(StringView(reinterpret_cast<const char8_t*>(RAPTOR_SOURCE_DIR)), u8"Data/Tests/Zip").AsView(), name);
    }
    StringView Text(const Array<byte>& bytes) { return StringView(reinterpret_cast<const utf8char*>(bytes.Data()), bytes.Size()); }
    const ZipEntry* Named(const ZipArchive& zip, StringView name)
    {
        for (const ZipEntry& e : zip.Entries())
            if (e.name.AsView() == name)
                return &e;
        return nullptr;
    }
    String Scene()
    {
        String text;
        for (int i = 1; i <= 200; ++i)
            text += Format(u8"line {} of a scene that compresses well\n", i).AsView();
        return text;
    }
}

TEST_CASE("zip: CRC-32 of known strings")
{
    const auto crc = [](StringView s) { return Crc32(Span<const byte>(reinterpret_cast<const byte*>(s.Data()), s.Size())); };
    CHECK(crc(u8"") == 0u);
    CHECK(crc(u8"123456789") == 0xCBF43926u);
    CHECK(crc(u8"The quick brown fox jumps over the lazy dog") == 0x414FA339u);
    // In two parts, as one.
    const StringView all = u8"123456789";
    const u32 first = Crc32(Span<const byte>(reinterpret_cast<const byte*>(all.Data()), 4));
    CHECK(Crc32(Span<const byte>(reinterpret_cast<const byte*>(all.Data()) + 4, 5), first) == 0xCBF43926u);
}

TEST_CASE("zip: entry names that stay in their folder")
{
    CHECK(IsSafeEntryName(u8"Game/Project.xml"));
    CHECK(IsSafeEntryName(u8"Game/"));
    CHECK(IsSafeEntryName(u8"a..b/c"));
    CHECK_FALSE(IsSafeEntryName(u8""));
    CHECK_FALSE(IsSafeEntryName(u8"../outside.txt"));
    CHECK_FALSE(IsSafeEntryName(u8"Game/../../x"));
    CHECK_FALSE(IsSafeEntryName(u8"Game\\..\\..\\x"));
    CHECK_FALSE(IsSafeEntryName(u8"/etc/passwd"));
    CHECK_FALSE(IsSafeEntryName(u8"C:/Windows/x"));
    CHECK_FALSE(IsSafeEntryName(u8".."));
}

TEST_CASE("zip: a project zipped by zip reads, each entry inflated or stored and checked")
{
    FileStream file(Fixture(u8"project.zip").AsView(), FileMode::Read);
    REQUIRE(file.IsValid());
    ZipArchive zip;
    String error;
    REQUIRE_MESSAGE(zip.Open(file, error), reinterpret_cast<const char*>(error.CStr()));
    CHECK(zip.Entries().Size() == 7u);
    const ZipEntry* scene = Named(zip, u8"Game/Scenes/Level1.scene");
    const ZipEntry* noise = Named(zip, u8"Game/Audio/noise.bin");
    const ZipEntry* empty = Named(zip, u8"Game/Empty.txt");
    const ZipEntry* folder = Named(zip, u8"Game/Scenes/");
    REQUIRE(scene != nullptr);
    REQUIRE(noise != nullptr);
    REQUIRE(empty != nullptr);
    REQUIRE(folder != nullptr);
    CHECK(scene->method == 8);
    CHECK(noise->method == 0);
    CHECK(folder->directory);
    CHECK(zip.TotalSize() == scene->size + noise->size + 23u);

    Array<byte> bytes;
    REQUIRE(zip.Read(*scene, bytes, error));
    CHECK(Text(bytes) == Scene().AsView());
    REQUIRE(zip.Read(*noise, bytes, error));
    CHECK(bytes.Size() == 3000u);
    REQUIRE(zip.Read(*empty, bytes, error));
    CHECK(bytes.IsEmpty());
    REQUIRE(zip.Read(*Named(zip, u8"Game/Project.xml"), bytes, error));
    CHECK(Text(bytes) == StringView(u8"<Project name=\"Game\"/>\n"));
}

TEST_CASE("zip: extracting writes every file under the folder and reports its progress")
{
    const String into = PathJoin(GetCurrentDirectory().AsView(), u8"scratch_zip_extract");
    (void)RemoveDirectoryRecursive(into.AsView());
    FileStream file(Fixture(u8"project.zip").AsView(), FileMode::Read);
    ZipArchive zip;
    String error;
    REQUIRE(zip.Open(file, error));
    u64 last = 0, total = 0;
    u32 calls = 0;
    REQUIRE_MESSAGE(zip.ExtractAll(into.AsView(), error, [&](u64 done, u64 all)
                                   {
                                       CHECK(done >= last);
                                       last = done;
                                       total = all;
                                       ++calls;
                                   }),
                    reinterpret_cast<const char*>(error.CStr()));
    CHECK(calls == 4u); // the four files; folders say nothing
    CHECK(last == total);
    CHECK(total == zip.TotalSize());
    Result<Array<byte>> scene = ReadFile(PathJoin(into.AsView(), u8"Game/Scenes/Level1.scene").AsView());
    REQUIRE(scene.HasValue());
    CHECK(Text(scene.Value()) == Scene().AsView());
    CHECK(FileExists(PathJoin(into.AsView(), u8"Game/Empty.txt").AsView()));
    CHECK(DirectoryExists(PathJoin(into.AsView(), u8"Game/Audio").AsView()));

    // A stored archive (zip -0) extracts the same files.
    FileStream stored(Fixture(u8"stored.zip").AsView(), FileMode::Read);
    ZipArchive plain;
    REQUIRE(plain.Open(stored, error));
    for (const ZipEntry& e : plain.Entries())
        CHECK(e.method == 0);
    Array<byte> bytes;
    REQUIRE(plain.Read(*Named(plain, u8"Game/Scenes/Level1.scene"), bytes, error));
    CHECK(Text(bytes) == Scene().AsView());
    (void)RemoveDirectoryRecursive(into.AsView());
}

TEST_CASE("zip: a name outside the folder refuses the archive before anything is written")
{
    const String into = PathJoin(GetCurrentDirectory().AsView(), u8"scratch_zip_escape/inner");
    (void)RemoveDirectoryRecursive(PathParent(into.AsView()));
    FileStream file(Fixture(u8"escape.zip").AsView(), FileMode::Read);
    ZipArchive zip;
    String error;
    REQUIRE(zip.Open(file, error));
    CHECK_FALSE(zip.ExtractAll(into.AsView(), error));
    CHECK(error.AsView().ContainsIgnoreCase(u8"../outside.txt"));
    CHECK_FALSE(DirectoryExists(into.AsView()));
    CHECK_FALSE(FileExists(PathJoin(PathParent(into.AsView()), u8"outside.txt").AsView()));
}

TEST_CASE("zip: a damaged entry, a comment, and what is not a zip")
{
    String error;
    {
        FileStream file(Fixture(u8"damaged.zip").AsView(), FileMode::Read);
        ZipArchive zip;
        REQUIRE(zip.Open(file, error));
        Array<byte> bytes;
        CHECK_FALSE(zip.Read(zip.Entries()[0], bytes, error));
        CHECK(error.AsView().ContainsIgnoreCase(u8"damaged"));
        CHECK(bytes.IsEmpty());
    }
    {
        // The end record found behind a 1000-byte comment.
        FileStream file(Fixture(u8"commented.zip").AsView(), FileMode::Read);
        ZipArchive zip;
        REQUIRE(zip.Open(file, error));
        Array<byte> bytes;
        REQUIRE(zip.Read(zip.Entries()[0], bytes, error));
        CHECK(Text(bytes) == StringView(u8"after a comment"));
    }
    {
        MemoryStream junk;
        const StringView text = u8"this is a text file, long enough to look for an end record in it";
        (void)junk.Write(text.Data(), text.Size());
        ZipArchive zip;
        CHECK_FALSE(zip.Open(junk, error));
        CHECK(error.AsView().ContainsIgnoreCase(u8"not a zip"));
    }
}
