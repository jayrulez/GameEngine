// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Core - :system partition
//
// Platform abstraction. Exports thin wrappers that forward to the per-platform
// backend (see SystemBackend.h). Higher-level subsystems (IO, Threading,
// Library) build on these primitives.

module;
#include "Core/Prelude.h"
#include "Core/System/SystemBackend.h"

// SystemBackend.h defines constants (kInvalidSocket) that are TU-local but safe
// to re-export as inline constexpr from the module interface.
#if defined(__clang__)
#pragma clang diagnostic push
// -WTU-local-entity-exposure only exists on newer clang (22+); ignore the
// unknown-option error first so older clang (21, Linux) still builds under -Werror.
#pragma clang diagnostic ignored "-Wunknown-warning-option"
#pragma clang diagnostic ignored "-WTU-local-entity-exposure"
#endif

export module foundation.core:system;

import :base;
import :allocator;
import :string;
import :path;
import :array;
import :span;

namespace foundation::core::detail
{
    // Ensure a StringView is null-terminated for C APIs. If the view is
    // already backed by a null-terminated String, the copy is unnecessary
    // but harmless. A future optimisation could check the trailing byte.
    struct NullTerminated
    {
        String storage;
        explicit NullTerminated(StringView sv) : storage(sv) {}
        [[nodiscard]] const char* CStr() const noexcept
        {
            return reinterpret_cast<const char*>(storage.CStr());
        }
    };

}

export namespace foundation::core
{
    // --- Time --------------------------------------------------------------

    // High-resolution monotonic counter; pair with GetTickFrequency().
    [[nodiscard]] inline u64 GetTicks() noexcept { return sys::GetTicks(); }

    [[nodiscard]] inline u64 GetTickFrequency() noexcept { return sys::GetTickFrequency(); }

    [[nodiscard]] inline f64 TicksToSeconds(u64 ticks) noexcept
    {
        return static_cast<f64>(ticks) / static_cast<f64>(sys::GetTickFrequency());
    }

    [[nodiscard]] inline f64 TicksToMilliseconds(u64 ticks) noexcept
    {
        return TicksToSeconds(ticks) * 1000.0;
    }

    inline void SleepMilliseconds(u32 milliseconds) noexcept
    {
        sys::SleepMilliseconds(milliseconds);
    }

    // --- Crash reporting ----------------------------------------------------

    // Install fatal-signal handlers that print the native backtrace to stderr before dying
    // (the assert path already prints one; without this a raw SIGSEGV prints nothing).
    // Idempotent; hosts call it once at startup. See SystemBackend.h.
    inline void InstallCrashBacktrace() noexcept { sys::InstallCrashBacktrace(); }

    // --- System info -------------------------------------------------------

    [[nodiscard]] inline u32 LogicalCoreCount() noexcept { return sys::LogicalCoreCount(); }

    [[nodiscard]] inline usize PageSize() noexcept { return sys::PageSize(); }

    // --- Virtual memory (page-granular) ------------------------------------

    // Reserves and commits `size` bytes of page-aligned memory; nullptr on
    // failure. Free with PageFree using the same size.
    [[nodiscard]] inline void* PageAllocate(usize size) noexcept { return sys::PageAllocate(size); }

    inline void PageFree(void* pointer, usize size) noexcept { sys::PageFree(pointer, size); }

    // --- Files (low-level primitives; IO wraps these) ----------------------

    using FileHandle = sys::FileHandle;
    using FileMode = sys::FileMode;
    using SeekOrigin = sys::SeekOrigin;
    inline constexpr FileHandle kInvalidFile = sys::kInvalidFile;

    [[nodiscard]] inline FileHandle FileOpen(StringView path, FileMode mode) noexcept
    {
        return sys::FileOpen(detail::NullTerminated(path).CStr(), mode);
    }

    [[nodiscard]] inline bool FileIsValid(FileHandle handle) noexcept
    {
        return handle != kInvalidFile;
    }

    inline void FileClose(FileHandle handle) noexcept { sys::FileClose(handle); }

    // Returns bytes transferred, or -1 on error.
    [[nodiscard]] inline i64 FileRead(FileHandle handle, void* buffer, u64 bytes) noexcept
    {
        return sys::FileRead(handle, buffer, bytes);
    }

    [[nodiscard]] inline i64 FileWrite(FileHandle handle, const void* buffer, u64 bytes) noexcept
    {
        return sys::FileWrite(handle, buffer, bytes);
    }

    [[nodiscard]] inline i64 FileSeek(FileHandle handle, i64 offset, SeekOrigin origin) noexcept
    {
        return sys::FileSeek(handle, offset, origin);
    }

    [[nodiscard]] inline i64 FileSize(FileHandle handle) noexcept { return sys::FileSize(handle); }

    /// An exclusive lock on an open file, without waiting: true when taken, false when another
    /// handle holds it, in this process or another. It lasts until FileUnlock, the handle's close,
    /// or the process's end (a crash included), so a lock is never left behind. Advisory: it
    /// stops other lockers, not readers or writers. For one-writer files (a tool's library).
    [[nodiscard]] inline bool FileTryLock(FileHandle handle) noexcept { return sys::FileTryLock(handle); }
    inline void FileUnlock(FileHandle handle) noexcept { sys::FileUnlock(handle); }

    [[nodiscard]] inline bool FileExists(StringView path) noexcept
    {
        return sys::FileExists(detail::NullTerminated(path).CStr());
    }

    /// The current OS process id (ops tooling: a hung process is killed by pid).
    [[nodiscard]] inline u64 ProcessId() noexcept { return sys::ProcessId(); }

    /// Fill `out` with OS entropy: the source for secrets and ids nobody can predict (a
    /// seeded Random is for reproducible generation). False when the OS refused.
    [[nodiscard]] inline bool FillEntropy(Span<byte> out) noexcept
    {
        return sys::FillEntropy(out.Data(), out.Size());
    }

    /// Absolute path of the running executable (the OS query, not argv[0], which can be
    /// bare or relative). Empty on failure.
    [[nodiscard]] inline String ExecutablePath()
    {
        char buffer[4096];
        const usize n = sys::ExecutablePath(buffer, sizeof(buffer));
        return String(StringView(reinterpret_cast<const utf8char*>(buffer), n));
    }

    inline bool FileDelete(StringView path) noexcept
    {
        return sys::FileDelete(detail::NullTerminated(path).CStr());
    }
    /// Copy preserving permissions (staged executables keep their +x bit). Overwrites.
    inline bool FileCopyPreserving(StringView from, StringView to) noexcept
    {
        return sys::FileCopyPreserving(detail::NullTerminated(from).CStr(),
                                       detail::NullTerminated(to).CStr());
    }

    /// Rename or move a file or directory (same volume).
    inline bool FileMove(StringView from, StringView to) noexcept
    {
        return sys::FileMove(detail::NullTerminated(from).CStr(),
                             detail::NullTerminated(to).CStr());
    }

    /// File size and last-write time (seconds since epoch). False when `path` is not a regular file.
    [[nodiscard]] inline bool FileStat(StringView path, u64& outSize, i64& outModifiedTime) noexcept
    {
        unsigned long long size = 0;
        long long mtime = 0;
        if (!sys::FileStat(detail::NullTerminated(path).CStr(), size, mtime))
        {
            return false;
        }
        outSize = static_cast<u64>(size);
        outModifiedTime = static_cast<i64>(mtime);
        return true;
    }

    [[nodiscard]] inline bool DirectoryExists(StringView path) noexcept
    {
        return sys::DirectoryExists(detail::NullTerminated(path).CStr());
    }
    inline bool CreateDirectory(StringView path) noexcept
    {
        return sys::CreateDirectory(detail::NullTerminated(path).CStr());
    }
    /// Recursive mkdir: creates every missing segment (true if the full path exists after).
    inline bool CreateDirectories(StringView path) noexcept
    {
        if (path.IsEmpty())
        {
            return false;
        }
        const utf8char* d = path.Data();
        for (usize i = 1; i < path.Size(); ++i)
        {
            if (d[i] == u8'/' || d[i] == u8'\\')
            {
                if (i > 0 && (d[i - 1] == u8'/' || d[i - 1] == u8'\\' || d[i - 1] == u8':'))
                {
                    continue;
                }
                if (!CreateDirectory(path.SubStr(0, i)))
                {
                    return false;
                }
            }
        }
        return CreateDirectory(path);
    }
    inline bool RemoveDirectory(StringView path) noexcept
    {
        return sys::RemoveDirectory(detail::NullTerminated(path).CStr());
    }

    // Lists immediate children of a directory, invoking `cb(ctx, name, isDir)`
    // per entry (excluding "." and ".."). `name` is a UTF-8 view valid only for
    // the duration of the call. Returns false if the directory can't be opened.
    using DirEntryCallback = void (*)(void* ctx, StringView name, bool isDirectory);
    inline bool ListDirectory(StringView path, DirEntryCallback cb, void* ctx) noexcept
    {
        struct Bridge
        {
            DirEntryCallback cb;
            void* ctx;
        } bridge{cb, ctx};
        return sys::ListDirectory(
            detail::NullTerminated(path).CStr(),
            [](void* c, const char* name, bool isDir) noexcept
            {
                auto* b = static_cast<Bridge*>(c);
                b->cb(b->ctx, StringView(reinterpret_cast<const utf8char*>(name)), isDir);
            },
            &bridge);
    }

    // --- Console -----------------------------------------------------------

    inline void ConsoleWrite(StringView text) noexcept
    {
        sys::ConsoleWrite(reinterpret_cast<const char*>(text.Data()), text.Size());
    }

    inline void ConsoleWriteError(StringView text) noexcept
    {
        sys::ConsoleWriteError(reinterpret_cast<const char*>(text.Data()), text.Size());
    }

    // --- Dynamic libraries (raw; the Library module wraps these) -----------

    using LibraryHandle = sys::LibraryHandle;

    [[nodiscard]] inline LibraryHandle OpenLibrary(StringView path) noexcept
    {
        return sys::LibraryOpen(detail::NullTerminated(path).CStr());
    }
    [[nodiscard]] inline void* GetLibrarySymbol(LibraryHandle handle, StringView name) noexcept
    {
        return sys::LibrarySymbol(handle, detail::NullTerminated(name).CStr());
    }
    inline void CloseLibrary(LibraryHandle handle) noexcept { sys::LibraryClose(handle); }

    // --- Environment -------------------------------------------------------

    // Value of environment variable `name`, or empty (unset) when it is not defined. Platform-
    // abstracted (Linux getenv / Win32 GetEnvironmentVariableA); copy the result if you need it past
    // the next environment mutation.
    [[nodiscard]] inline Optional<String> GetEnvironmentVariable(StringView name)
    {
        detail::NullTerminated n(name);
        char buffer[1024];
        const usize length = sys::GetEnvironmentVariable(n.CStr(), buffer, sizeof(buffer));
        if (length == 0)
        {
            return {};
        } // unset (or empty, treated the same for our uses)
        // Env values in practice are short (paths); anything past the buffer is truncated.
        const usize got = (length < sizeof(buffer)) ? length : sizeof(buffer) - 1;
        return String(StringView(reinterpret_cast<const utf8char*>(buffer), got));
    }

    // --- User data directory ----------------------------------------------

    // User-data directory for `appName`: the platform base directory (resolved per platform in the
    // backend: $XDG_DATA_HOME or ~/.local/share on Linux, %LOCALAPPDATA% on Windows,
    // ~/Library/Application Support on macOS) with `appName` appended. Falls back to the bare app
    // name when the base can't be resolved. Global editor settings and the default export templates
    // root live here. The default folder name, USER_DATA_DIR_NAME, is baked in by CMake
    // (USER_DATA_DIR_NAME_VALUE on the policy target); there is no in-source default, so the name
    // lives only in the build system.
#ifndef USER_DATA_DIR_NAME
#error "USER_DATA_DIR_NAME is not defined - set USER_DATA_DIR_NAME_VALUE in the root CMakeLists (policy target)"
#endif
    [[nodiscard]] inline String GetUserDataDirectory(StringView appName = USER_DATA_DIR_NAME)
    {
        char buffer[1024];
        const usize length = sys::GetUserDataDirectory(buffer, sizeof(buffer));
        if (length == 0 || length >= sizeof(buffer))
        {
            return String(appName);
        } // unresolved/truncated
        return PathJoin(StringView(reinterpret_cast<const utf8char*>(buffer), length), appName);
    }

    // What is written under the user data directory is durable at once on a desktop. In a browser
    // it is in memory until pushed to the page's storage (IndexedDB, mounted over the directory by
    // the web player before the game starts): call this after writing a file there that must
    // outlive the page, a save or a settings file. Returns at once; the push finishes in the
    // background. A no-op everywhere but the web.
    inline void PersistUserData() noexcept { sys::PersistUserData(); }

    // --- Platform identity -------------------------------------------------

    // Host platform tag ("Win64" / "Linux64" / "Mac64"), matching the Bin/<Config>/<Platform> layout.
    [[nodiscard]] inline StringView GetHostPlatformName() noexcept
    {
        return StringView(reinterpret_cast<const utf8char*>(sys::GetHostPlatformName()));
    }

    // Build config the running tool was compiled with ("Debug" / "Release" / "RelWithDebInfo"),
    // matching the Bin/<Config>/... layout. The export host template stamps itself with this so a
    // Debug editor synthesizes a Debug template.
#ifndef BUILD_CONFIG
#define BUILD_CONFIG "Release"
#endif
#ifndef BUILD_COMPILER
#define BUILD_COMPILER ""
#endif
    [[nodiscard]] inline StringView GetBuildConfigName() noexcept
    {
        return StringView(reinterpret_cast<const utf8char*>(BUILD_CONFIG));
    }

    // Compiler that built the running tool ("Clang" / "GCC" / "MSVC"; empty if unknown). Metadata
    // only: recorded in the host template for traceability, never a selector.
    [[nodiscard]] inline StringView GetBuildCompilerName() noexcept
    {
        return StringView(reinterpret_cast<const utf8char*>(BUILD_COMPILER));
    }

    // Platform executable filename for `baseName`: appends this platform's exe extension
    // ("Game" -> "Game.exe" on Windows, "Game" elsewhere).
    [[nodiscard]] inline String GetExecutableName(StringView baseName)
    {
        String name(baseName);
        name += StringView(reinterpret_cast<const utf8char*>(sys::ExecutableExtension()));
        return name;
    }

    // Absolute path of the running executable (empty on failure).
    [[nodiscard]] inline String GetExecutablePath()
    {
        char buffer[4096];
        const usize length = sys::GetExecutablePath(buffer, sizeof(buffer));
        if (length == 0)
        {
            return String{};
        }
        const usize got = (length < sizeof(buffer)) ? length : sizeof(buffer) - 1;
        return String(StringView(reinterpret_cast<const utf8char*>(buffer), got));
    }

    // Current working directory (empty on failure). Used to resolve a relative path to absolute.
    [[nodiscard]] inline String GetCurrentDirectory()
    {
        char buffer[4096];
        const usize length = sys::GetCurrentDirectory(buffer, sizeof(buffer));
        if (length == 0)
        {
            return String{};
        }
        const usize got = (length < sizeof(buffer)) ? length : sizeof(buffer) - 1;
        return String(StringView(reinterpret_cast<const utf8char*>(buffer), got));
    }

    // Directory containing the running executable (its parent), empty on failure. This is the
    // Bin/<Config>/<Platform> dir - where sibling tools (e.g. Engine.Player) live.
    [[nodiscard]] inline String GetExecutableDirectory()
    {
        const String path = GetExecutablePath();
        usize slash = 0;
        bool found = false;
        for (usize i = 0; i < path.Size(); ++i)
        {
            if (path[i] == utf8char('/') || path[i] == utf8char('\\'))
            {
                slash = i;
                found = true;
            }
        }
        return found ? String(path.AsView().SubStr(0, slash)) : String{};
    }

    // Open a directory in the OS file manager (Explorer / xdg-open). Non-blocking; true once the
    // launch was initiated, false if it could not start or `path` is empty. See the backend note.
    inline bool OpenPathInFileManager(StringView path) noexcept
    {
        return sys::OpenPathInFileManager(detail::NullTerminated(path).CStr());
    }

    // Normalize a path's directory separators to the OS-native form (see the backend: '/' -> '\' on
    // Windows, '\' -> '/' on POSIX). Copies into `out` (capacity `cap`, NUL-terminated); false (no
    // write) if empty or overlong-with-NUL. Tests cover the per-platform behaviour through it, and the
    // Win32 OpenPathInFileManager uses it so Explorer, which rejects '/', gets native separators.
    [[nodiscard]] inline bool NormalizePathSeparators(StringView path, char* out, usize cap) noexcept
    {
        return sys::NormalizePathSeparators(detail::NullTerminated(path).CStr(), out, cap);
    }

    // Outcome of RunProcess.
    struct ProcessResult
    {
        int exitCode = -1;    // >=0 is the child's exit code; <0 means it could not spawn or was killed by a signal
        String output;        // combined stdout and stderr (captured, capped)
        [[nodiscard]] bool Ran() const noexcept { return exitCode >= 0; }
        [[nodiscard]] bool Ok() const noexcept { return exitCode == 0; }
    };

    // Run `exe` (an explicit path: no shell, no PATH search) with `args` (each one whole argument,
    // no splitting or globbing). Blocks until the child exits, capturing its combined stdout and
    // stderr. For cook-time tool shell-outs (the WGSL cook's naga and tint). Do not call it on the UI
    // thread.
    [[nodiscard]] inline ProcessResult RunProcess(StringView exe, Span<const StringView> args)
    {
        const String exeZ(exe);
        Array<String> store;
        store.Reserve(args.Size());
        for (usize i = 0; i < args.Size(); ++i)
        {
            store.PushBack(String(args[i]));
        }
        Array<const char*> ptrs;
        ptrs.Reserve(store.Size());
        for (usize i = 0; i < store.Size(); ++i)
        {
            ptrs.PushBack(reinterpret_cast<const char*>(store[i].CStr()));
        }

        // Cook diagnostics are small; 64 KiB caps a runaway tool without truncating real errors.
        Array<char> buffer;
        buffer.Resize(64 * 1024);
        const int code = sys::RunProcess(reinterpret_cast<const char*>(exeZ.CStr()), ptrs.Data(),
                                         static_cast<int>(ptrs.Size()), buffer.Data(),
                                         buffer.Size());
        ProcessResult result;
        result.exitCode = code;
        result.output = String(reinterpret_cast<const char8_t*>(buffer.Data()));
        return result;
    }

    // --- UDP sockets (IPv4); the foundation.net datagram backend wraps these ---
    using SocketHandle = sys::SocketHandle;
    inline constexpr SocketHandle kInvalidSocket = sys::kInvalidSocket;

    inline bool InitializeNetworking() noexcept { return sys::InitializeNetworking(); }
    inline void ShutdownNetworking() noexcept { sys::ShutdownNetworking(); }

    [[nodiscard]] inline SocketHandle UdpOpen(u16 port, u16* outBoundPort = nullptr) noexcept
    {
        return sys::UdpOpen(port, outBoundPort);
    }
    inline void SocketClose(SocketHandle socket) noexcept { sys::SocketClose(socket); }

    [[nodiscard]] inline bool ParseIPv4(StringView dottedQuad, u32& outIp) noexcept
    {
        return sys::ParseIPv4(detail::NullTerminated(dottedQuad).CStr(), &outIp);
    }
    /// A host name or a dotted-quad literal to a host-order IPv4 address (the literal parses
    /// without a lookup; a name goes through the platform resolver and blocks for its
    /// duration, so resolve at connect time, never per frame). False when it does not resolve.
    [[nodiscard]] inline bool ResolveHostIPv4(StringView host, u32& outIp) noexcept
    {
        return sys::ResolveHostIPv4(detail::NullTerminated(host).CStr(), &outIp);
    }

    [[nodiscard]] inline i64 UdpSendTo(SocketHandle socket, u32 ip, u16 port, const void* data,
                                       usize size) noexcept
    {
        return sys::UdpSendTo(socket, ip, port, data, size);
    }
    [[nodiscard]] inline i64 UdpRecvFrom(SocketHandle socket, void* out, usize outCap, u32& fromIp,
                                         u16& fromPort) noexcept
    {
        return sys::UdpRecvFrom(socket, out, outCap, &fromIp, &fromPort);
    }

    // --- TCP (stream) sockets; the foundation.http, websocket and debugger transports wrap these ---
    /// A listening socket on `port` (0 = OS-assigned). `loopbackOnly` binds 127.0.0.1 alone (a
    /// local tool's server); otherwise every interface (a game server). *outBoundIp is the bound
    /// IPv4 address in host order, 0 for every interface.
    [[nodiscard]] inline SocketHandle TcpListen(u16 port, bool loopbackOnly = false,
                                                u16* outBoundPort = nullptr,
                                                u32* outBoundIp = nullptr) noexcept
    {
        return sys::TcpListen(port, loopbackOnly, outBoundPort, outBoundIp);
    }
    [[nodiscard]] inline SocketHandle TcpAccept(SocketHandle listener, u32* fromIp = nullptr,
                                                u16* fromPort = nullptr) noexcept
    {
        return sys::TcpAccept(listener, fromIp, fromPort);
    }
    [[nodiscard]] inline SocketHandle TcpConnect(u32 ip, u16 port) noexcept
    {
        return sys::TcpConnect(ip, port);
    }
    [[nodiscard]] inline int TcpConnectStatus(SocketHandle socket) noexcept
    {
        return sys::TcpConnectStatus(socket);
    }
    [[nodiscard]] inline i64 TcpSend(SocketHandle socket, const void* data, usize size) noexcept
    {
        return sys::TcpSend(socket, data, size);
    }
    [[nodiscard]] inline i64 TcpRecv(SocketHandle socket, void* out, usize outCap) noexcept
    {
        return sys::TcpRecv(socket, out, outCap);
    }
}
