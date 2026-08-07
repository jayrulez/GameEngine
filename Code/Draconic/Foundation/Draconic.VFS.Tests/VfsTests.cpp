#include <doctest/doctest.h>

#include "Draconic.Foundation/Prelude.h"

import draconic.foundation;
import draconic.vfs;

using namespace draconic::foundation;
using namespace draconic::vfs;

TEST_CASE("vfs: NativeFileSystem read + scheme-routed VirtualFileSystem")
{
    const StringView file = u8"draconic_vfs_test.tmp";
    const byte data[] = {byte{7}, byte{8}, byte{9}};
    REQUIRE(WriteFile(file, Span<const byte>{data, ArrayCount(data)}).IsOk());

    NativeFileSystem native(u8".");
    CHECK(native.Exists(u8"draconic_vfs_test.tmp"));
    CHECK_FALSE(native.Exists(u8"draconic_vfs_nope.xyz"));
    {
        UniquePtr<IStream> stream = native.Open(u8"draconic_vfs_test.tmp", FileMode::Read);
        REQUIRE(static_cast<bool>(stream));
        byte buffer[3] = {};
        CHECK(stream->Read(buffer, 3) == 3u);
        CHECK(buffer[0] == byte{7});
        CHECK(buffer[2] == byte{9});
    }

    // Mount under a scheme; address as "assets://...".
    VirtualFileSystem vfs;
    vfs.Mount(u8"assets", native);
    CHECK(vfs.GetMount(u8"assets") == &native);
    CHECK(vfs.GetMount(u8"missing") == nullptr);
    CHECK(vfs.Exists(u8"assets://draconic_vfs_test.tmp"));
    CHECK_FALSE(vfs.Exists(u8"assets://nope.xyz"));
    CHECK_FALSE(vfs.Exists(u8"unmounted://whatever"));
    CHECK_FALSE(vfs.Exists(u8"schemeless/path")); // no scheme -> rejected
    {
        UniquePtr<IStream> stream = vfs.Open(u8"assets://draconic_vfs_test.tmp", FileMode::Read);
        REQUIRE(static_cast<bool>(stream));
        byte b = byte{0};
        CHECK(stream->Read(&b, 1) == 1u);
        CHECK(b == byte{7});
    }
    CHECK_FALSE(static_cast<bool>(vfs.Open(u8"unmounted://x", FileMode::Read)));

    CHECK(FileDelete(file));
}

TEST_CASE("vfs: capability queries via As*()")
{
    NativeFileSystem native(u8".");
    IFileSystem& fs = native;

    REQUIRE(fs.AsEnumerable() != nullptr);
    REQUIRE(fs.AsWritable() != nullptr);
    REQUIRE(fs.AsStat() != nullptr);
    REQUIRE(fs.AsWatchable() != nullptr);

    // A router advertises no capabilities of its own.
    VirtualFileSystem vfs;
    CHECK(vfs.AsEnumerable() == nullptr);
    CHECK(vfs.AsWritable() == nullptr);
}

TEST_CASE("vfs: writable + enumerable round-trip")
{
    NativeFileSystem native(u8".");
    IWritableFileSystem* w = native.AsWritable();
    IEnumerableFileSystem* e = native.AsEnumerable();
    REQUIRE(w != nullptr);
    REQUIRE(e != nullptr);

    // Save creates intermediate directories.
    const byte payload[] = {byte{1}, byte{2}, byte{3}, byte{4}};
    REQUIRE(
        w->Save(u8"draconic_vfs_dir/sub/blob.bin", Span<const byte>{payload, ArrayCount(payload)})
            .IsOk());
    CHECK(native.Exists(u8"draconic_vfs_dir/sub/blob.bin"));

    // Enumerate the subfolder; the blob is listed and is not a directory.
    Array<DirEntry> entries;
    REQUIRE(e->Enumerate(u8"draconic_vfs_dir/sub", entries).IsOk());
    bool foundBlob = false;
    for (const DirEntry& entry : entries)
    {
        if (entry.name == u8"blob.bin")
        {
            foundBlob = true;
            CHECK_FALSE(entry.isDirectory);
        }
    }
    CHECK(foundBlob);

    // Enumerate the parent; "sub" is listed as a directory.
    Array<DirEntry> parent;
    REQUIRE(e->Enumerate(u8"draconic_vfs_dir", parent).IsOk());
    bool foundSub = false;
    for (const DirEntry& entry : parent)
    {
        if (entry.name == u8"sub")
        {
            foundSub = true;
            CHECK(entry.isDirectory);
        }
    }
    CHECK(foundSub);

    // Enumerate of a missing folder fails.
    Array<DirEntry> missing;
    CHECK_FALSE(e->Enumerate(u8"draconic_vfs_dir/nope", missing).IsOk());

    // Cleanup.
    CHECK(w->Delete(u8"draconic_vfs_dir/sub/blob.bin").IsOk());
    CHECK(RemoveDirectory(u8"draconic_vfs_dir/sub"));
    CHECK(RemoveDirectory(u8"draconic_vfs_dir"));
}

TEST_CASE("vfs: NativeFileSystem stat reports size + modified time")
{
    NativeFileSystem native(u8".");
    IWritableFileSystem* w = native.AsWritable();
    IStatFileSystem* st = native.AsStat();
    REQUIRE(w != nullptr);
    REQUIRE(st != nullptr);

    const byte payload[5] = {byte{1}, byte{2}, byte{3}, byte{4}, byte{5}};
    REQUIRE(w->Save(u8"vfs_stat_test.bin", Span<const byte>(payload, 5)).IsOk());

    FileStatInfo info;
    REQUIRE(st->Stat(u8"vfs_stat_test.bin", info));
    CHECK(info.size == 5u);
    CHECK(info.modifiedTime > 0); // a plausible wall-clock epoch time

    // Rewriting changes the size; mtime moves monotonically (>=, same-second writes allowed).
    const i64 firstTime = info.modifiedTime;
    REQUIRE(w->Save(u8"vfs_stat_test.bin", Span<const byte>(payload, 3)).IsOk());
    REQUIRE(st->Stat(u8"vfs_stat_test.bin", info));
    CHECK(info.size == 3u);
    CHECK(info.modifiedTime >= firstTime);

    // Missing files and directories are not regular files.
    CHECK_FALSE(st->Stat(u8"vfs_stat_missing.bin", info));
    CHECK_FALSE(st->Stat(u8"", info));

    REQUIRE(w->Delete(u8"vfs_stat_test.bin").IsOk());
}

TEST_CASE("vfs: NativeFileSystem change source detects adds, edits, and removals")
{
    const StringView dir = u8"draconic_vfs_watch_dir";
    NativeFileSystem cleaner(dir);
    {
        Array<DirEntry> entries;
        if (cleaner.AsEnumerable()->Enumerate(u8"sub", entries).IsOk())
        {
            for (const DirEntry& e : entries)
            {
                (void)cleaner.AsWritable()->Delete(PathJoin(u8"sub", e.name.AsView()).AsView());
            }
        }
        entries.Clear();
        if (cleaner.AsEnumerable()->Enumerate(u8"", entries).IsOk())
        {
            for (const DirEntry& e : entries)
            {
                if (!e.isDirectory)
                {
                    (void)cleaner.AsWritable()->Delete(e.name.AsView());
                }
            }
        }
    }
    (void)CreateDirectory(dir);

    NativeFileSystem fs(dir);
    IWatchableFileSystem* watchable = fs.AsWatchable();
    REQUIRE(watchable != nullptr);
    IChangeSource* source = watchable->ChangeSource();
    REQUIRE(source != nullptr);

    // A pre-existing file is part of the baseline, not a change.
    const byte a[2] = {byte{1}, byte{2}};
    REQUIRE(fs.AsWritable()->Save(u8"before.bin", Span<const byte>(a, 2)).IsOk());
    source->Track(u8"");

    Array<String> changed;
    CHECK_FALSE(source->Poll(changed));
    CHECK(changed.IsEmpty());

    // Added file (incl. inside a subdirectory) is reported once, then goes quiet.
    REQUIRE(fs.AsWritable()->Save(u8"sub/new.bin", Span<const byte>(a, 2)).IsOk());
    CHECK(source->Poll(changed));
    REQUIRE(changed.Size() == 1u);
    CHECK(changed[0] == StringView(u8"sub/new.bin"));
    changed.Clear();
    CHECK_FALSE(source->Poll(changed));

    // Content edit (different size, so the stat diff can't false-negative on same-second mtime).
    const byte b[5] = {byte{1}, byte{2}, byte{3}, byte{4}, byte{5}};
    REQUIRE(fs.AsWritable()->Save(u8"before.bin", Span<const byte>(b, 5)).IsOk());
    changed.Clear();
    CHECK(source->Poll(changed));
    REQUIRE(changed.Size() == 1u);
    CHECK(changed[0] == StringView(u8"before.bin"));

    // Removal is reported.
    REQUIRE(fs.AsWritable()->Delete(u8"sub/new.bin").IsOk());
    changed.Clear();
    CHECK(source->Poll(changed));
    REQUIRE(changed.Size() == 1u);
    CHECK(changed[0] == StringView(u8"sub/new.bin"));

    // Untrack: edits go unreported.
    source->Untrack(u8"");
    REQUIRE(fs.AsWritable()->Save(u8"before.bin", Span<const byte>(a, 2)).IsOk());
    changed.Clear();
    CHECK_FALSE(source->Poll(changed));

    (void)fs.AsWritable()->Delete(u8"before.bin");
    (void)RemoveDirectory(PathJoin(dir, u8"sub"));
    (void)RemoveDirectory(dir);
}

// --- SourcePath (draconic.vfs:source_path) ---------------------------------------------

TEST_CASE("vfs.sourcePath: normalization table")
{
    // Backslashes heal, duplicate slashes and '.' segments collapse, trailing slash drops.
    CHECK(SourcePath(u8"Fonts\\Roboto.ttf").View() == u8"Fonts/Roboto.ttf");
    CHECK(SourcePath(u8"Fonts//Sub///Roboto.ttf").View() == u8"Fonts/Sub/Roboto.ttf");
    CHECK(SourcePath(u8"./Fonts/./Roboto.ttf").View() == u8"Fonts/Roboto.ttf");
    CHECK(SourcePath(u8"Fonts/Roboto.ttf/").View() == u8"Fonts/Roboto.ttf");
    CHECK(SourcePath(u8"Roboto.ttf").View() == u8"Roboto.ttf");
    CHECK(SourcePath(u8"").IsEmpty());

    // Contract violations normalize to EMPTY - a missing reference, never a wrong one.
    CHECK(SourcePath(u8"/abs/path.ttf").IsEmpty());          // absolute
    CHECK(SourcePath(u8"\\abs\\path.ttf").IsEmpty());        // absolute (backslash)
    CHECK(SourcePath(u8"../escape.ttf").IsEmpty());          // escapes the mount
    CHECK(SourcePath(u8"Fonts/../../up.ttf").IsEmpty());     // buried escape
    CHECK(SourcePath(u8"c:/windows/f.ttf").IsEmpty());       // volume
    CHECK(SourcePath(u8"file://x/f.ttf").IsEmpty());         // scheme
}

TEST_CASE("vfs.sourcePath: accessors")
{
    const SourcePath p(u8"Fonts/Sub/Roboto-Regular.TTF");
    CHECK(p.FileName() == u8"Roboto-Regular.TTF");
    CHECK(p.Stem() == u8"Roboto-Regular");
    CHECK(p.Extension().AsView() == u8"ttf"); // lowercased
    CHECK(p.Directory() == u8"Fonts/Sub");

    const SourcePath flat(u8"Roboto.ttf");
    CHECK(flat.Directory() == u8"");
    CHECK(flat.FileName() == u8"Roboto.ttf");

    const SourcePath noExt(u8"Fonts/README");
    CHECK(noExt.Extension().IsEmpty());
    CHECK(noExt.Stem() == u8"README");

    // Comparison is case-SENSITIVE everywhere (one rule; lint catches case bugs).
    CHECK(SourcePath(u8"a.ttf") != SourcePath(u8"A.ttf"));
    CHECK(SourcePath(u8"A.ttf") < SourcePath(u8"a.ttf"));
    CHECK(SourcePath(u8"a/b.ttf") == SourcePath(u8"a\\b.ttf"));
}

TEST_CASE("vfs.sourcePath: wire shape is String-compatible (existing data loads)")
{
    // Write a plain String field, read it back as a SourcePath - the exact upgrade an
    // existing .xasset/.rasset goes through. Windows-authored separators heal on read.
    MemoryStream stream;
    {
        BinarySerializer ar(stream, SerializeMode::Write);
        String legacy(u8"Fonts\\Roboto.ttf");
        Serialize(ar, "fileName", legacy);
    }
    (void)stream.Seek(0, SeekOrigin::Begin);
    {
        BinarySerializer ar(stream, SerializeMode::Read);
        SourcePath upgraded;
        Serialize(ar, "fileName", upgraded);
        CHECK(upgraded.View() == u8"Fonts/Roboto.ttf");
    }

    // And the reverse: a SourcePath field reads back as a String unchanged.
    MemoryStream reverse;
    {
        BinarySerializer ar(reverse, SerializeMode::Write);
        SourcePath path(u8"Fonts/Roboto.ttf");
        Serialize(ar, "fileName", path);
    }
    (void)reverse.Seek(0, SeekOrigin::Begin);
    {
        BinarySerializer ar(reverse, SerializeMode::Read);
        String back;
        Serialize(ar, "fileName", back);
        CHECK(back.AsView() == u8"Fonts/Roboto.ttf");
    }
}
