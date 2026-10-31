// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

#include <doctest/doctest.h>

#include <cstring>
#if !defined(_WIN32)
#include <sys/stat.h> // stat/chmod: FileCopyPreserving's +x-preservation check
#endif

#include "Core/Debug/Assert.h"
#include "Core/Log/Log.h"
#include "Core/Reflection/Reflect.h"

import foundation.core;

using namespace foundation::core;

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
    const StringView path = u8"scratch_system_test.tmp";
    const char payload[] = "engine file IO";
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
        CHECK(buffer[0] == 'e');

        // Seek back to a known offset and re-read.
        CHECK(FileSeek(f, 7, SeekOrigin::Begin) == 7);
        char c = 0;
        CHECK(FileRead(f, &c, 1) == 1);
        CHECK(c == 'f'); // "engine file IO"[7]

        FileClose(f);
    }

    CHECK(FileDelete(path));
    CHECK_FALSE(FileExists(path));
}

TEST_CASE("system: a file lock is held by one handle at a time, and the file stays readable")
{
    const StringView path = u8"scratch_system_lock.tmp";
    {
        FileHandle f = FileOpen(path, FileMode::Write);
        REQUIRE(FileIsValid(f));
        CHECK(FileWrite(f, "1234\n", 5) == 5);
        FileClose(f);
    }
    FileHandle first = FileOpen(path, FileMode::ReadWrite);
    FileHandle second = FileOpen(path, FileMode::ReadWrite);
    REQUIRE(FileIsValid(first));
    REQUIRE(FileIsValid(second));
    CHECK(FileTryLock(first));
    CHECK_FALSE(FileTryLock(second)); // another handle, even in this process

    // The locked file's own bytes still read through another handle (what a second tool reads
    // to say who holds it).
    FileHandle reader = FileOpen(path, FileMode::Read);
    REQUIRE(FileIsValid(reader));
    char buffer[8] = {};
    CHECK(FileRead(reader, buffer, 5) == 5);
    CHECK(buffer[0] == '1');
    FileClose(reader);

    FileUnlock(first);
    CHECK(FileTryLock(second)); // released: the other takes it
    CHECK_FALSE(FileTryLock(first));
    FileClose(second); // closing releases it too
    CHECK(FileTryLock(first));
    FileClose(first);
    CHECK_FALSE(FileTryLock(kInvalidFile));
    CHECK(FileDelete(path));
}

TEST_CASE("system: opening a missing file fails cleanly")
{
    FileHandle f = FileOpen(u8"scratch_definitely_missing.xyz", FileMode::Read);
    CHECK_FALSE(FileIsValid(f));
}

TEST_CASE("system: console write does not crash")
{
    ConsoleWrite(u8"[console-test] console output check\n");
    CHECK(true);
}

TEST_CASE("system: OpenPathInFileManager rejects an empty path without launching")
{
    // Guard only: an empty or whitespace path must return false and not spawn a process. A real
    // path is not exercised here because it would pop a file-manager window during the test run.
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
    const StringView root = u8"scratch_sys_mkdirs";
    CHECK(CreateDirectories(u8"scratch_sys_mkdirs/a/b/c"));
    CHECK(DirectoryExists(u8"scratch_sys_mkdirs/a/b/c"));
    CHECK(CreateDirectories(u8"scratch_sys_mkdirs/a/b/c")); // idempotent
    (void)RemoveDirectory(u8"scratch_sys_mkdirs/a/b/c");
    (void)RemoveDirectory(u8"scratch_sys_mkdirs/a/b");
    (void)RemoveDirectory(u8"scratch_sys_mkdirs/a");
    (void)RemoveDirectory(root);
}

TEST_CASE("system: FileCopyPreserving copies bytes and keeps the mode")
{
    const StringView src = u8"scratch_sys_copy_src.bin";
    const StringView dst = u8"scratch_sys_copy_dst.bin";
    {
        FileHandle f = FileOpen(src, FileMode::Write);
        REQUIRE(FileIsValid(f));
        const char payload[] = "exec-me";
        (void)FileWrite(f, payload, sizeof(payload));
        FileClose(f);
    }
#if !defined(_WIN32)
    (void)::chmod("scratch_sys_copy_src.bin", 0755); // the +x bit the copy must keep
#endif
    CHECK(FileCopyPreserving(src, dst));
    u64 srcSize = 0, dstSize = 0;
    i64 t = 0;
    CHECK(FileStat(src, srcSize, t));
    CHECK(FileStat(dst, dstSize, t));
    CHECK(srcSize == dstSize);
#if !defined(_WIN32)
    struct stat st{};
    REQUIRE(::stat("scratch_sys_copy_dst.bin", &st) == 0);
    CHECK((st.st_mode & 0111) != 0); // execute bits preserved
#endif
    (void)FileDelete(src);
    (void)FileDelete(dst);
}

// --- RunProcess (blocking spawn + capture) ---------------------------------
// Subprocess spawning is a desktop-only capability (RunProcess drives the cook-time naga/tint
// shell-outs). Web has no fork/exec, so these do not apply there.
#if !PLATFORM_WEB

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
    // Explicit path, no PATH search: a bogus name cannot resolve on any platform.
    const ProcessResult r = RunProcess(u8"scratch_no_such_binary_zzz", Span<const StringView>());
    CHECK_FALSE(r.Ran());
    CHECK(r.exitCode < 0);
}

#endif // !PLATFORM_WEB (subprocess spawning is desktop-only)

// NormalizePathSeparators is fanned out to the platform backends; the Win32 OpenPathInFileManager
// relies on it (Explorer refuses '/'). The direction is OS-native and therefore host-dependent
// ('\' on Windows, '/' on POSIX), so the separator expectations are compiled per platform. The
// empty, exact-fit and overlong contract is shared and asserted once.
TEST_CASE("system: NormalizePathSeparators yields OS-native separators")
{
    char out[64];

#if PLATFORM_WINDOWS
    SUBCASE("forward slashes become backslashes on Windows")
    {
        CHECK(NormalizePathSeparators(u8"a/b/My Dir", out, sizeof(out)));
        CHECK(std::strcmp(out, "a\\b\\My Dir") == 0);
    }
    SUBCASE("already-native path is unchanged")
    {
        CHECK(NormalizePathSeparators(u8"C:\\home\\foo", out, sizeof(out)));
        CHECK(std::strcmp(out, "C:\\home\\foo") == 0);
    }
#else
    SUBCASE("backslashes become forward slashes on POSIX")
    {
        CHECK(NormalizePathSeparators(u8"a\\b\\My Dir", out, sizeof(out)));
        CHECK(std::strcmp(out, "a/b/My Dir") == 0);
    }
    SUBCASE("already-native path is unchanged")
    {
        CHECK(NormalizePathSeparators(u8"/home/foo/bar", out, sizeof(out)));
        CHECK(std::strcmp(out, "/home/foo/bar") == 0);
    }
#endif
    SUBCASE("empty path is rejected")
    {
        CHECK_FALSE(NormalizePathSeparators(u8"", out, sizeof(out)));
    }
    SUBCASE("overlong path is rejected, not truncated mid-path")
    {
        // 8 chars + NUL needs 9; a capacity of 8 must refuse rather than write a partial path.
        char small[8];
        CHECK_FALSE(NormalizePathSeparators(u8"a\\bcdefg", small, sizeof(small)));
    }
    SUBCASE("exact fit (length + NUL == cap) is accepted")
    {
        char exact[8];
        CHECK(NormalizePathSeparators(u8"a\\bcdef", exact, sizeof(exact))); // 7 + NUL == 8
#if PLATFORM_WINDOWS
        CHECK(std::strcmp(exact, "a\\bcdef") == 0);
#else
        CHECK(std::strcmp(exact, "a/bcdef") == 0);
#endif
    }
}

#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
#include <sys/wait.h>
#include <unistd.h>

// The fatal-signal backtrace: a child installs the handler and segfaults; the parent asserts
// that the crash banner and backtrace reached stderr and that the default signal disposition was
// preserved (the child still dies by the signal, so cores and exit status behave as usual).
TEST_CASE("system: InstallCrashBacktrace prints a native stack on SIGSEGV")
{
#if defined(__SANITIZE_ADDRESS__) || (defined(__has_feature) && __has_feature(address_sanitizer))
    // Under AddressSanitizer InstallCrashBacktrace is a no-op: the sanitizer's own handler
    // reports the faulting frame and the freeing stack, and ours would replace it.
    MESSAGE("AddressSanitizer build: the engine's crash handler yields to the sanitizer's");
    return;
#endif
    int pipeFds[2];
    REQUIRE(pipe(pipeFds) == 0);

    const pid_t child = fork();
    REQUIRE(child >= 0);
    if (child == 0)
    {
        // Child: route stderr into the pipe, install, crash.
        dup2(pipeFds[1], 2);
        close(pipeFds[0]);
        close(pipeFds[1]);
        InstallCrashBacktrace();
        volatile int* nullPointer = nullptr;
        *nullPointer = 42; // SIGSEGV: the handler prints, then re-raises with SIG_DFL
        _exit(0);          // unreachable
    }

    close(pipeFds[1]);
    char buffer[8192];
    usize total = 0;
    ssize_t n = 0;
    while ((n = read(pipeFds[0], buffer + total, sizeof(buffer) - 1 - total)) > 0)
    {
        total += static_cast<usize>(n);
        if (total >= sizeof(buffer) - 1)
        {
            break;
        }
    }
    buffer[total] = '\0';
    close(pipeFds[0]);

    int status = 0;
    REQUIRE(waitpid(child, &status, 0) == child);
    CHECK(WIFSIGNALED(status));                       // default disposition preserved
    CHECK(WTERMSIG(status) == SIGSEGV);               // died by the segfault, after printing
    CHECK(std::strstr(buffer, "SIGSEGV") != nullptr); // the banner
    CHECK(std::strstr(buffer, "backtrace") != nullptr);
    CHECK(std::strstr(buffer, "0x") != nullptr); // at least one frame line
}
#endif

TEST_CASE("system: FillEntropy fills the whole span with bytes that never repeat a draw")
{
    byte first[32] = {};
    byte second[32] = {};
    REQUIRE(FillEntropy(Span<byte>{first, sizeof(first)}));
    REQUIRE(FillEntropy(Span<byte>{second, sizeof(second)}));
    CHECK(std::memcmp(first, second, sizeof(first)) != 0);
    bool anyNonZero = false;
    for (byte b : first)
    {
        anyNonZero = anyNonZero || b != byte{0};
    }
    CHECK(anyNonZero);
    byte none[1] = {byte{7}};
    CHECK(FillEntropy(Span<byte>{none, 0})); // an empty span is trivially filled
    // Past the OS call's per-request cap (getentropy takes 256 at a time) the fill chunks:
    // every part of a long buffer, the tail included, is written.
    byte many[300];
    std::memset(many, 0, sizeof(many));
    REQUIRE(FillEntropy(Span<byte>{many, sizeof(many)}));
    bool headWritten = false;
    bool tailWritten = false;
    for (usize i = 0; i < 256; ++i)
    {
        headWritten = headWritten || many[i] != byte{0};
    }
    for (usize i = 256; i < sizeof(many); ++i)
    {
        tailWritten = tailWritten || many[i] != byte{0};
    }
    CHECK(headWritten);
    CHECK(tailWritten); // 44 zero bytes of entropy: odds of 1 in 2^352
    CHECK(none[0] == byte{7});
}

