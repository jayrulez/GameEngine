// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The importer registry and the shared import helpers: extension routing, path helpers, and the
// copy-into-Sources step every file importer builds on.

#include <doctest/doctest.h>

#include "Core/Prelude.h"
#include <initializer_list>

import foundation.core;
import foundation.content;
import pipeline.importer;

using namespace foundation::core;
using namespace pipeline;

namespace
{
    class FakeImporter final : public IFileImporter
    {
    public:
        [[nodiscard]] StringView Label() const override { return u8"Fake"; }
        [[nodiscard]] bool Accepts(StringView extension) const override
        {
            return extension == StringView(u8"fak");
        }
        [[nodiscard]] Result<foundation::content::Instance*>
        Import(StringView, const ImportContext&, foundation::content::Group&,
               const pipeline::ImportOptions*, Object*,
               Array<DeferredImportWrite>*) override
        {
            return Err(ErrorCode::NotSupported);
        }
    };

    // A second importer that also claims "fak", for the multi-match chooser path (FindAllFor).
    class FakeImporter2 final : public IFileImporter
    {
    public:
        [[nodiscard]] StringView Label() const override { return u8"Fake2"; }
        [[nodiscard]] bool Accepts(StringView extension) const override
        {
            return extension == StringView(u8"fak");
        }
        [[nodiscard]] Result<foundation::content::Instance*>
        Import(StringView, const ImportContext&, foundation::content::Group&,
               const pipeline::ImportOptions*, Object*, Array<DeferredImportWrite>*) override
        {
            return Err(ErrorCode::NotSupported);
        }
    };
}

TEST_CASE("importer: path helpers")
{
    CHECK(FileExtensionLower(u8"/a/b/Foo.PNG") == StringView(u8"png"));
    CHECK(FileExtensionLower(u8"C:\\art\\thing.Jpeg") == StringView(u8"jpeg"));
    CHECK(FileExtensionLower(u8"noext") == StringView(u8""));
    CHECK(FileExtensionLower(u8"/dotted.dir/noext") == StringView(u8""));

    CHECK(FileNameOf(u8"/a/b/foo.png") == StringView(u8"foo.png"));
    CHECK(FileNameOf(u8"C:\\a\\b.png") == StringView(u8"b.png"));
    CHECK(FileNameOf(u8"bare.png") == StringView(u8"bare.png"));

    CHECK(FileStemOf(u8"foo.png") == StringView(u8"foo"));
    CHECK(FileStemOf(u8"foo") == StringView(u8"foo"));
}

TEST_CASE("importer: registry routes by extension, first match wins")
{
    ImporterRegistry registry{DefaultAllocator()};
    CHECK(registry.FindFor(u8"fak") == nullptr);

    registry.Register(
        UniquePtr<IFileImporter>(DefaultAllocator().New<FakeImporter>(), DefaultAllocator()));
    CHECK(registry.Count() == 1u);

    IFileImporter* importer = registry.FindFor(u8"fak");
    REQUIRE(importer != nullptr);
    CHECK(importer->Label() == StringView(u8"Fake"));
    CHECK(registry.FindFor(u8"png") == nullptr);
}

TEST_CASE("importer: FindAllFor returns every match (registration order) for the chooser")
{
    ImporterRegistry registry{DefaultAllocator()};
    CHECK(registry.FindAllFor(u8"fak").IsEmpty());

    registry.Register(
        UniquePtr<IFileImporter>(DefaultAllocator().New<FakeImporter>(), DefaultAllocator()));
    registry.Register(
        UniquePtr<IFileImporter>(DefaultAllocator().New<FakeImporter2>(), DefaultAllocator()));

    Array<IFileImporter*> matches = registry.FindAllFor(u8"fak");
    REQUIRE(matches.Size() == 2u);
    CHECK(matches[0]->Label() == StringView(u8"Fake")); // registration order preserved
    CHECK(matches[1]->Label() == StringView(u8"Fake2"));
    CHECK(registry.FindAllFor(u8"png").IsEmpty());
}

TEST_CASE("importer: CopyIntoSources lands the bytes in the project's Sources tree")
{
    const StringView dir = u8"scratch_importer_test_project";
    // Clean slate.
    FileDelete(PathJoin(dir, u8"Project.xml"));
    FileDelete(PathJoin(dir, u8"Sources/payload.bin"));
    for (StringView sub : {u8"Content", u8"Sources", u8"Cooked", u8"Editor", u8".cache"})
    {
        RemoveDirectory(PathJoin(dir, sub));
    }
    RemoveDirectory(dir);

    // CopyIntoSources needs only a sources directory, not a project or an editor, so the test
    // provides a bare scratch tree.
    const String sourcesRoot = PathJoin(dir, u8"Sources");
    REQUIRE(CreateDirectories(sourcesRoot.AsView()));

    // A loose OS file to import.
    const byte payload[4] = {byte{9}, byte{8}, byte{7}, byte{6}};
    REQUIRE(WriteFile(u8"scratch_importer_loose.bin", Span<const byte>(payload, 4)).IsOk());

    Result<String> name = CopyIntoSources(ImportContext{DefaultAllocator(), sourcesRoot.AsView()}, u8"scratch_importer_loose.bin");
    REQUIRE(name.HasValue());
    CHECK(name.Value() == StringView(u8"scratch_importer_loose.bin"));

    const String copied = PathJoin(sourcesRoot.AsView(), name.Value().AsView());
    Result<Array<byte>> bytes = ReadFile(copied.AsView());
    REQUIRE(bytes.HasValue());
    CHECK(bytes.Value().Size() == 4u);
    CHECK(bytes.Value()[0] == byte{9});

    // Reimporting the same name reuses the existing copy (no error).
    CHECK(CopyIntoSources(ImportContext{DefaultAllocator(), sourcesRoot.AsView()}, u8"scratch_importer_loose.bin").HasValue());
    // Missing source is a clean failure.
    CHECK_FALSE(CopyIntoSources(ImportContext{DefaultAllocator(), sourcesRoot.AsView()}, u8"scratch_importer_missing.bin").HasValue());

    FileDelete(u8"scratch_importer_loose.bin");
    FileDelete(copied.AsView());
    FileDelete(PathJoin(dir, u8"Project.xml"));
    for (StringView sub : {u8"Content", u8"Sources", u8"Cooked", u8"Editor", u8".cache"})
    {
        RemoveDirectory(PathJoin(dir, sub));
    }
    RemoveDirectory(dir);
}

// The import ledger and the copy into Sources/: a second file of
// the same name from another folder never overwrites the first's copy, it takes "a.2.png"; the
// first file imported again (changed) overwrites its own copy; the same bytes again are shared,
// untouched. Without a ledger, the caller decides.
TEST_CASE("importer: a copy never overwrites another file's; a re-import overwrites its own")
{
    const String dir = PathJoin(GetCurrentDirectory().AsView(), u8"scratch_importer_ledger");
    (void)RemoveDirectoryRecursive(dir.AsView());
    const String sources = PathJoin(dir.AsView(), u8"Sources");
    const String one = PathJoin(dir.AsView(), u8"one");
    const String two = PathJoin(dir.AsView(), u8"two");
    for (const String* d : {&sources, &one, &two})
    {
        REQUIRE(CreateDirectories(d->AsView()));
    }
    auto write = [](const String& folder, StringView text)
    {
        const String path = PathJoin(folder.AsView(), u8"a.png");
        REQUIRE(WriteFile(path.AsView(), Span<const byte>(reinterpret_cast<const byte*>(text.Data()), text.Size())).IsOk());
        return path;
    };
    auto read = [&](StringView name)
    {
        Result<Array<byte>> bytes = ReadFile(PathJoin(sources.AsView(), name).AsView());
        REQUIRE(bytes.HasValue());
        return String(StringView(reinterpret_cast<const utf8char*>(bytes.Value().Data()), bytes.Value().Size()));
    };
    ImportContext context{DefaultAllocator(), sources.AsView()};
    context.ledgerPath = ImportLedgerPath(PathJoin(dir.AsView(), u8".cache").AsView());

    const String first = write(one, u8"first");
    Result<String> a = CopyIntoSources(context, first.AsView(), false);
    REQUIRE(a.HasValue());
    CHECK(a.Value() == StringView(u8"a.png"));
    CHECK(ImportOriginOf(context.ledgerPath.AsView(), u8"a.png") == first);

    // Another folder's a.png: beside it, the first's copy untouched.
    const String second = write(two, u8"second");
    Result<String> b = CopyIntoSources(context, second.AsView(), true); // even when the caller would allow it
    REQUIRE(b.HasValue());
    CHECK(b.Value() == StringView(u8"a.2.png"));
    CHECK(read(u8"a.png") == StringView(u8"first"));
    CHECK(read(u8"a.2.png") == StringView(u8"second"));

    // The first, changed, imported again: its own copy overwritten.
    (void)write(one, u8"first, edited");
    Result<String> again = CopyIntoSources(context, first.AsView(), false);
    REQUIRE(again.HasValue());
    CHECK(again.Value() == StringView(u8"a.png"));
    CHECK(read(u8"a.png") == StringView(u8"first, edited"));
    // The second again, unchanged: the same copy.
    CHECK(CopyIntoSources(context, second.AsView(), false).Value() == StringView(u8"a.2.png"));
    // Named without copying (a deferred write's), from the ledger alone.
    CHECK(SourcesNameFor(context, second.AsView(), false) == StringView(u8"a.2.png"));

    // No ledger: a copy there of unknown origin is overwritten only when the caller allows it.
    ImportContext bare{DefaultAllocator(), sources.AsView()};
    const String third = PathJoin(dir.AsView(), u8"third.png");
    REQUIRE(WriteFile(third.AsView(), Span<const byte>(reinterpret_cast<const byte*>(u8"x"), 1)).IsOk());
    REQUIRE(CopyIntoSources(bare, third.AsView()).HasValue()); // third.png, new
    REQUIRE(WriteFile(third.AsView(), Span<const byte>(reinterpret_cast<const byte*>(u8"y"), 1)).IsOk());
    CHECK(CopyIntoSources(bare, third.AsView(), false).Value() == StringView(u8"third.2.png"));
    CHECK(CopyIntoSources(bare, third.AsView(), true).Value() == StringView(u8"third.png"));
    (void)RemoveDirectoryRecursive(dir.AsView());
}

TEST_CASE("importer: the ledger keeps one line per file, the last origin")
{
    const String ledger = PathJoin(GetCurrentDirectory().AsView(), u8"scratch_importer_ledger_lines/.cache/imports.txt");
    (void)FileDelete(ledger.AsView());
    CHECK(ImportOriginOf(ledger.AsView(), u8"a.png").IsEmpty());
    RecordImportOrigin(ledger.AsView(), u8"a.png", u8"/one/a.png");
    RecordImportOrigin(ledger.AsView(), u8"b.png", u8"/one/b.png");
    RecordImportOrigin(ledger.AsView(), u8"a.png", u8"/two/a.png");
    CHECK(ImportOriginOf(ledger.AsView(), u8"a.png") == StringView(u8"/two/a.png"));
    CHECK(ImportOriginOf(ledger.AsView(), u8"b.png") == StringView(u8"/one/b.png"));
    Result<Array<byte>> bytes = ReadFile(ledger.AsView());
    REQUIRE(bytes.HasValue());
    usize lines = 0;
    for (const byte c : bytes.Value())
        lines += c == byte{'\n'} ? 1u : 0u;
    CHECK(lines == 2u);
    CHECK(ImportOriginOf({}, u8"a.png").IsEmpty()); // no ledger: says nothing
}
