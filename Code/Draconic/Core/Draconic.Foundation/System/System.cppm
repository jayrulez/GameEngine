// Draconic Foundation - :system partition
//
// Platform abstraction. Exports thin wrappers that forward to the per-platform
// backend (see SystemBackend.h). Higher-level subsystems (IO, Threading,
// Library) build on these primitives.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/System/SystemBackend.h"

// SystemBackend.h defines constants (kInvalidSocket) that are TU-local but safe
// to re-export as inline constexpr from the module interface.
#if defined(__clang__)
#pragma clang diagnostic push
// -WTU-local-entity-exposure only exists on newer clang (22+); ignore the
// unknown-option error first so older clang (21, Linux) still builds under -Werror.
#pragma clang diagnostic ignored "-Wunknown-warning-option"
#pragma clang diagnostic ignored "-WTU-local-entity-exposure"
#endif

export module draconic.foundation:system;

import :base;
import :allocator;
import :string;
import :path;
import :array;
import :span;

namespace draconic::foundation::detail
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

export namespace draconic::foundation
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

    [[nodiscard]] inline bool FileExists(StringView path) noexcept
    {
        return sys::FileExists(detail::NullTerminated(path).CStr());
    }

    inline bool FileDelete(StringView path) noexcept
    {
        return sys::FileDelete(detail::NullTerminated(path).CStr());
    }
    /// Rename/move a file OR directory (same volume).
    /// Copy PRESERVING permissions (staged executables keep their +x bit). Overwrites.
    inline bool FileCopyPreserving(StringView from, StringView to) noexcept
    {
        return sys::FileCopyPreserving(detail::NullTerminated(from).CStr(),
                                       detail::NullTerminated(to).CStr());
    }

    inline bool FileMove(StringView from, StringView to) noexcept
    {
        return sys::FileMove(detail::NullTerminated(from).CStr(),
                             detail::NullTerminated(to).CStr());
    }

    /// File size + last-write time (seconds since epoch). False when `path` is not a regular file.
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
        } // unset (or empty - treated the same for our uses)
        // Env values in practice are short (paths); anything past the buffer is truncated.
        const usize got = (length < sizeof(buffer)) ? length : sizeof(buffer) - 1;
        return String(StringView(reinterpret_cast<const utf8char*>(buffer), got));
    }

    // --- User data directory ----------------------------------------------

    // User-data directory for `appName` (default "draconic"): the platform base directory (resolved
    // per-platform in the backend - $XDG_DATA_HOME/~/.local/share on Linux, %LOCALAPPDATA% on Windows,
    // ~/Library/Application Support on macOS) with `appName` appended. Falls back to the bare app name
    // when the base can't be resolved. Where global editor settings live - and the default export
    // templates root (docs/design/export.md §5).
    [[nodiscard]] inline String GetUserDataDirectory(StringView appName = u8"draconic")
    {
        char buffer[1024];
        const usize length = sys::GetUserDataDirectory(buffer, sizeof(buffer));
        if (length == 0 || length >= sizeof(buffer))
        {
            return String(appName);
        } // unresolved/truncated
        return PathJoin(StringView(reinterpret_cast<const utf8char*>(buffer), length), appName);
    }

    // --- Platform identity -------------------------------------------------

    // Host platform tag ("Win64" / "Linux64" / "Mac64"), matching the Bin/<Config>/<Platform> layout.
    [[nodiscard]] inline StringView GetHostPlatformName() noexcept
    {
        return StringView(reinterpret_cast<const utf8char*>(sys::GetHostPlatformName()));
    }

    // Build config the running tool was compiled with ("Debug" / "Release" / "RelWithDebInfo"),
    // matching the Bin/<Config>/... layout - the export host template stamps itself with this so a
    // Debug editor synthesizes a Debug template (export-templates.md, the config axis).
#ifndef DRACONIC_BUILD_CONFIG
#define DRACONIC_BUILD_CONFIG "Release"
#endif
#ifndef DRACONIC_BUILD_COMPILER
#define DRACONIC_BUILD_COMPILER ""
#endif
    [[nodiscard]] inline StringView GetBuildConfigName() noexcept
    {
        return StringView(reinterpret_cast<const utf8char*>(DRACONIC_BUILD_CONFIG));
    }

    // Compiler that built the running tool ("Clang" / "GCC" / "MSVC"; empty if unknown). Metadata
    // only - recorded in the host template for traceability, never a selector.
    [[nodiscard]] inline StringView GetBuildCompilerName() noexcept
    {
        return StringView(reinterpret_cast<const utf8char*>(DRACONIC_BUILD_COMPILER));
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
    // Bin/<Config>/<Platform> dir - where sibling tools (e.g. Draconic.Engine.Player) live.
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

    // Outcome of RunProcess.
    struct ProcessResult
    {
        int exitCode = -1;    // >=0 = child exit code; <0 = could not spawn OR killed by a signal
        String output;        // combined stdout+stderr (captured, capped)
        [[nodiscard]] bool Ran() const noexcept { return exitCode >= 0; }
        [[nodiscard]] bool Ok() const noexcept { return exitCode == 0; }
    };

    // Run `exe` (an explicit path - no shell, no PATH search) with `args` (each one whole argument,
    // no splitting/globbing). BLOCKS until the child exits, capturing its combined stdout+stderr. For
    // cook-time tool shell-outs (the WGSL cook's naga + tint). Do NOT call on the UI thread.
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

    // --- UDP sockets (IPv4) - the draconic.net datagram backend wraps these ---
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

    // --- TCP (stream) sockets - the draconic.http / websocket / debugger transports wrap these ---
    [[nodiscard]] inline SocketHandle TcpListen(u16 port, u16* outBoundPort = nullptr) noexcept
    {
        return sys::TcpListen(port, outBoundPort);
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
