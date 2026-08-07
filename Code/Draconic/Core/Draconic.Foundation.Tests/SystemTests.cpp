#include <doctest/doctest.h>

#include <cstring>
#if !defined(_WIN32)
#include <sys/stat.h> // stat/chmod: FileCopyPreserving's +x-preservation check
#endif

#include "Draconic.Foundation/Debug/Assert.h"
#include "Draconic.Foundation/Log/Log.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

import draconic.foundation;

using namespace draconic::foundation;

// --- System ----------------------------------------------------------------

TEST_CASE("system: high-resolution time advances")
{
    CHECK(GetTickFrequency() > 0u);

    const u64 t0 = GetTicks();
    SleepMilliseconds(2);
    const u64 t1 = GetTicks();

    CHECK(t1 > t0);

    const f64 seconds = TicksToSeconds(t1 - t0);
    CHECK(seconds > 0.0);
    CHECK(seconds < 1.0); // a 2ms sleep should be well under a second
    CHECK(TicksToMilliseconds(t1 - t0) >= 1.0);
}

TEST_CASE("system: info queries are sane")
{
    CHECK(LogicalCoreCount() >= 1u);

    const usize pageSize = PageSize();
    CHECK(pageSize >= 4096u);
    CHECK(IsPowerOfTwo(pageSize));
}

TEST_CASE("system: page allocation is usable and page-aligned")
{
    const usize pageSize = PageSize();

    void* p = PageAllocate(pageSize);
    REQUIRE(p != nullptr);
    CHECK(IsAligned(p, pageSize));

    MemSet(p, 0x5A, pageSize);
    CHECK(static_cast<u8*>(p)[0] == 0x5Au);
    CHECK(static_cast<u8*>(p)[pageSize - 1] == 0x5Au);

    PageFree(p, pageSize);
}

// --- System: files ---------------------------------------------------------

TEST_CASE("system: file write / read / seek / size round-trip")
{
    const StringView path = u8"draconic_system_test.tmp";
    const char payload[] = "Draconic file IO";
    const u64 length = sizeof(payload) - 1; // exclude null terminator

    // Write
    {
        FileHandle f = FileOpen(path, FileMode::Write);
        REQUIRE(FileIsValid(f));
        CHECK(FileWrite(f, payload, length) == static_cast<i64>(length));
        FileClose(f);
    }

    CHECK(FileExists(path));

    // Read back
    {
        FileHandle f = FileOpen(path, FileMode::Read);
        REQUIRE(FileIsValid(f));
        CHECK(FileSize(f) == static_cast<i64>(length));

        char buffer[32] = {};
        CHECK(FileRead(f, buffer, length) == static_cast<i64>(length));
        CHECK(buffer[0] == 'D');

        // Seek back to a known offset and re-read.
        CHECK(FileSeek(f, 7, SeekOrigin::Begin) == 7);
        char c = 0;
        CHECK(FileRead(f, &c, 1) == 1);
        CHECK(c == 'c'); // "Draconic file IO"[7]

        FileClose(f);
    }

    CHECK(FileDelete(path));
    CHECK_FALSE(FileExists(path));
}

TEST_CASE("system: opening a missing file fails cleanly")
{
    FileHandle f = FileOpen(u8"draconic_definitely_missing.xyz", FileMode::Read);
    CHECK_FALSE(FileIsValid(f));
}

TEST_CASE("system: console write does not crash")
{
    ConsoleWrite(u8"[draconic-test] console output check\n");
    CHECK(true);
}

TEST_CASE("system: OpenPathInFileManager rejects an empty path without launching")
{
    // Guard only - an empty/whitespace path must return false and NOT spawn a process. A real path is
    // deliberately not exercised here: it would pop a file-manager window during the test run.
    CHECK_FALSE(OpenPathInFileManager(u8""));
}

TEST_CASE("system: GetCurrentDirectory returns a non-empty absolute path")
{
    const String cwd = GetCurrentDirectory();
    CHECK_FALSE(cwd.IsEmpty());
    CHECK(PathIsAbsolute(cwd.AsView()));
}

TEST_CASE("system: CreateDirectories makes every missing segment")
{
    const StringView root = u8"draconic_sys_mkdirs";
    CHECK(CreateDirectories(u8"draconic_sys_mkdirs/a/b/c"));
    CHECK(DirectoryExists(u8"draconic_sys_mkdirs/a/b/c"));
    CHECK(CreateDirectories(u8"draconic_sys_mkdirs/a/b/c")); // idempotent
    (void)RemoveDirectory(u8"draconic_sys_mkdirs/a/b/c");
    (void)RemoveDirectory(u8"draconic_sys_mkdirs/a/b");
    (void)RemoveDirectory(u8"draconic_sys_mkdirs/a");
    (void)RemoveDirectory(root);
}

TEST_CASE("system: FileCopyPreserving copies bytes and keeps the mode")
{
    const StringView src = u8"draconic_sys_copy_src.bin";
    const StringView dst = u8"draconic_sys_copy_dst.bin";
    {
        FileHandle f = FileOpen(src, FileMode::Write);
        REQUIRE(FileIsValid(f));
        const char payload[] = "exec-me";
        (void)FileWrite(f, payload, sizeof(payload));
        FileClose(f);
    }
#if !defined(_WIN32)
    (void)::chmod("draconic_sys_copy_src.bin", 0755); // the +x bit the copy must keep
#endif
    CHECK(FileCopyPreserving(src, dst));
    u64 srcSize = 0, dstSize = 0;
    i64 t = 0;
    CHECK(FileStat(src, srcSize, t));
    CHECK(FileStat(dst, dstSize, t));
    CHECK(srcSize == dstSize);
#if !defined(_WIN32)
    struct stat st{};
    REQUIRE(::stat("draconic_sys_copy_dst.bin", &st) == 0);
    CHECK((st.st_mode & 0111) != 0); // execute bits preserved
#endif
    (void)FileDelete(src);
    (void)FileDelete(dst);
}

// --- RunProcess (blocking spawn + capture) ---------------------------------
// Subprocess spawning is a desktop-only capability (RunProcess drives the cook-time naga/tint
// shell-outs). Web has no fork/exec, so these do not apply there.
#if !DRACONIC_PLATFORM_WEB

TEST_CASE("system: RunProcess captures stdout and reports exit 0")
{
#if defined(_WIN32)
    const StringView exe = u8"C:\\Windows\\System32\\cmd.exe";
    const StringView args[] = {u8"/c", u8"echo hello"};
#else
    const StringView exe = u8"/bin/echo";
    const StringView args[] = {u8"hello"};
#endif
    const ProcessResult r =
        RunProcess(exe, Span<const StringView>(args, sizeof(args) / sizeof(args[0])));
    CHECK(r.Ran());
    CHECK(r.Ok());
    CHECK(r.exitCode == 0);
    // echo emits "hello" then a newline, so the capture begins with the token.
    CHECK(r.output.AsView().StartsWith(u8"hello"));
}

TEST_CASE("system: RunProcess propagates a non-zero exit code")
{
#if defined(_WIN32)
    const StringView exe = u8"C:\\Windows\\System32\\cmd.exe";
    const StringView args[] = {u8"/c", u8"exit 3"};
    const int expected = 3;
#else
    const StringView exe = u8"/bin/sh";
    const StringView args[] = {u8"-c", u8"exit 3"};
    const int expected = 3;
#endif
    const ProcessResult r = RunProcess(exe, Span<const StringView>(args, 2));
    CHECK(r.Ran());
    CHECK_FALSE(r.Ok());
    CHECK(r.exitCode == expected);
}

TEST_CASE("system: RunProcess reports failure to spawn a missing binary")
{
    // Explicit path, no PATH search - a bogus name cannot resolve on any platform.
    const ProcessResult r = RunProcess(u8"draconic_no_such_binary_zzz", Span<const StringView>());
    CHECK_FALSE(r.Ran());
    CHECK(r.exitCode < 0);
}

#endif // !DRACONIC_PLATFORM_WEB (subprocess spawning is desktop-only)
