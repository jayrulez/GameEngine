// Draconic Foundation - System backend (classic header).
//
// Platform-specific OS services are implemented in per-platform .cpp files
// (System/Linux, System/Win32) as plain external-linkage functions. The
// :system module partition exports thin wrappers that forward here, keeping OS
// headers out of the module BMI. Uses <cstdint>/<cstddef> types so it needs no
// module import; draconic::foundation's u64/usize are aliases of these exact types.

#ifndef DRACONIC_FOUNDATION_SYSTEM_BACKEND_H
#define DRACONIC_FOUNDATION_SYSTEM_BACKEND_H

#include <cstddef>
#include <cstdint>

namespace draconic::foundation::sys
{
    // --- Time --------------------------------------------------------------
    std::uint64_t GetTicks() noexcept;         // high-resolution monotonic counter
    std::uint64_t GetTickFrequency() noexcept; // counter ticks per second
    void SleepMilliseconds(std::uint32_t milliseconds) noexcept;

    // --- System info -------------------------------------------------------
    std::uint32_t LogicalCoreCount() noexcept;
    std::size_t PageSize() noexcept;

    // --- Native stack trace (assert/fatal reporting) ------------------------
    // Writes the current native call stack to file descriptor `fd` (frames print as
    // `binary(+0xADDR)`; resolve offline with `addr2line -e <binary> -f -C`). Returns the
    // frame count (0 where unsupported). Avoids heap allocation (assert-context safe).
    int WriteBacktrace(int fd) noexcept;

    // --- Environment -------------------------------------------------------
    // Copy environment variable `name` into `out` (truncated to outSize-1, always null-terminated
    // when out/outSize are valid). Returns the value's FULL length excluding the null - so a return
    // >= outSize signals truncation - or 0 when the variable is unset.
    std::size_t GetEnvironmentVariable(const char* name, char* out, std::size_t outSize) noexcept;

    // Platform user-data BASE directory (NO app name appended): $XDG_DATA_HOME or ~/.local/share
    // (Linux), %LOCALAPPDATA% (Windows), ~/Library/Application Support (macOS). Same truncation /
    // return contract as GetEnvironmentVariable; 0 when it cannot be resolved. The :system module
    // wrapper appends the application name.
    std::size_t GetUserDataDirectory(char* out, std::size_t outSize) noexcept;

    // --- Platform identity -------------------------------------------------
    // Host platform tag ("Win64" / "Linux64" / "Mac64"), matching the Bin/<Config>/<Platform> layout.
    const char* GetHostPlatformName() noexcept;
    // Executable filename extension for this platform, WITH the dot (".exe" on Windows, "" elsewhere).
    const char* ExecutableExtension() noexcept;
    // Absolute path of the running executable (readlink /proc/self/exe, GetModuleFileNameA). Same
    // truncation / return contract as GetEnvironmentVariable; 0 on failure.
    std::size_t GetExecutablePath(char* out, std::size_t outSize) noexcept;

    // Current working directory (getcwd / GetCurrentDirectoryA). Same truncation / return contract as
    // GetEnvironmentVariable; 0 on failure. Used to resolve project-relative paths to absolute.
    std::size_t GetCurrentDirectory(char* out, std::size_t outSize) noexcept;

    // --- OS integration ----------------------------------------------------
    // Open `path` (a directory) in the OS file manager: Explorer via CreateProcess on Windows,
    // xdg-open (double-forked) on Linux. NON-BLOCKING - launches detached and returns immediately;
    // true once the launch was initiated (not that the manager finished opening), false if it could
    // not be started or `path` is empty. Must never block the caller (it runs on the UI thread).
    bool OpenPathInFileManager(const char* path) noexcept;

    // Run `exe` with arguments argv[0..argc-1] (WITHOUT the program name - the backend prepends `exe`
    // as argv[0]). No shell, no PATH search - `exe` is an explicit path. BLOCKS until the child exits.
    // Returns the exit code (0..255), or -1 if the process could not be spawned OR was killed by a
    // signal. The child's combined stdout+stderr is captured into `out` (truncated to outCap-1, always
    // null-terminated when out/outCap are valid; pass out=nullptr/outCap=0 to discard) and its stdin is
    // empty (NUL). For cook-time tool shell-outs (the WGSL cook's naga + tint). WAITS, unlike
    // OpenPathInFileManager, so it must not run on the UI thread.
    int RunProcess(const char* exe, const char* const* argv, int argc, char* out,
                   std::size_t outCap) noexcept;

    // --- Virtual memory (page-granular) ------------------------------------
    void* PageAllocate(std::size_t size) noexcept; // nullptr on failure
    void PageFree(void* pointer, std::size_t size) noexcept;

    // --- Files (low-level primitives; IO wraps these) ----------------------
    // Opaque handle: fd on Linux, HANDLE on Win32. kInvalidFile on failure.
    using FileHandle = std::intptr_t;
    inline constexpr FileHandle kInvalidFile = -1;

    enum class FileMode
    {
        Read,      // existing file, read-only
        Write,     // create/truncate, write-only
        ReadWrite, // create if needed, read+write
        Append,    // create if needed, write at end
    };

    enum class SeekOrigin
    {
        Begin,
        Current,
        End,
    };

    FileHandle FileOpen(const char* path, FileMode mode) noexcept;
    void FileClose(FileHandle handle) noexcept;
    std::int64_t FileRead(FileHandle handle, void* buffer,
                          std::uint64_t bytes) noexcept; // -1 on error
    std::int64_t FileWrite(FileHandle handle, const void* buffer, std::uint64_t bytes) noexcept;
    std::int64_t FileSeek(FileHandle handle, std::int64_t offset,
                          SeekOrigin origin) noexcept; // new pos, -1 error
    std::int64_t FileSize(FileHandle handle) noexcept; // -1 on error
    bool FileExists(const char* path) noexcept;
    bool FileDelete(const char* path) noexcept;
    // Rename/move a file OR directory (same volume). True on success.
    bool FileMove(const char* from, const char* to) noexcept;
    // Copy PRESERVING permissions (staged executables keep +x; Windows CopyFileW does this
    // natively, POSIX re-applies the source mode). True on success; overwrites dst.
    bool FileCopyPreserving(const char* from, const char* to) noexcept;
    // File size + last-write time (seconds since epoch). False if the file doesn't exist.
    bool FileStat(const char* path, unsigned long long& outSize,
                  long long& outModifiedTime) noexcept;
    bool DirectoryExists(const char* path) noexcept;
    bool CreateDirectory(const char* path) noexcept; // true if created or already exists
    bool RemoveDirectory(const char* path) noexcept;

    // Lists the immediate children of a directory, invoking `cb` once per entry
    // (excluding "." and ".."). Allocation-free: the backend owns no buffers.
    // Returns false if the directory can't be opened.
    using DirEntryCallback = void (*)(void* ctx, const char* name, bool isDirectory) noexcept;
    bool ListDirectory(const char* path, DirEntryCallback cb, void* ctx) noexcept;

    // --- Console -----------------------------------------------------------
    void ConsoleWrite(const char* text, std::uint64_t length) noexcept;      // stdout
    void ConsoleWriteError(const char* text, std::uint64_t length) noexcept; // stderr

    // --- Dynamic libraries (raw; the Library module wraps these) -----------
    using LibraryHandle = void*; // HMODULE on Win32

    LibraryHandle LibraryOpen(const char* path) noexcept;                 // nullptr on failure
    void* LibrarySymbol(LibraryHandle handle, const char* name) noexcept; // nullptr if absent
    void LibraryClose(LibraryHandle handle) noexcept;

    // --- UDP sockets (IPv4; the draconic.net datagram backend wraps these) --
    // Opaque handle: a POSIX fd or a Win32 SOCKET. kInvalidSocket = failure.
    using SocketHandle = std::uintptr_t;
    constexpr SocketHandle kInvalidSocket = ~static_cast<SocketHandle>(0);

    // Bring up / tear down the platform networking layer (WSAStartup on Win32; no-op elsewhere).
    // Reference-counted + idempotent: call once per socket user, ShutdownNetworking when done.
    bool InitializeNetworking() noexcept;
    void ShutdownNetworking() noexcept;

    // Open a NON-BLOCKING UDP socket bound to `port` (0 = OS-assigned). Returns kInvalidSocket on
    // failure; *outBoundPort (if non-null) receives the actual bound port (host order).
    SocketHandle UdpOpen(std::uint16_t port, std::uint16_t* outBoundPort) noexcept;
    void SocketClose(SocketHandle socket) noexcept;

    // Parse "a.b.c.d" into a HOST-order IPv4 address. false on malformed input.
    bool ParseIPv4(const char* dottedQuad, std::uint32_t* outIp) noexcept;

    // Send `size` bytes to (ip host-order, port host-order). Returns bytes sent (>=0), or -1 on a
    // real error (a would-block on a full send buffer returns 0 - the caller may retry).
    std::int64_t UdpSendTo(SocketHandle socket, std::uint32_t ip, std::uint16_t port,
                           const void* data, std::size_t size) noexcept;
    // Receive one datagram into `out` (capacity outCap). Returns bytes read (>0), 0 when none are
    // pending (non-blocking), or -1 on error. Fills the sender's (ip,port) host-order when non-null.
    std::int64_t UdpRecvFrom(SocketHandle socket, void* out, std::size_t outCap,
                             std::uint32_t* fromIp, std::uint16_t* fromPort) noexcept;

    // --- TCP (stream) sockets (the draconic.http / websocket transports wrap these) --
    // All non-blocking. Consumers: HTTP client (draconic.http), a P5 WebSocket transport, and the
    // script debugger's remote transport. Not used by the UDP game networking.
    //
    // Open a listening socket on `port` (0 = OS-assigned); *outBoundPort gets the actual port.
    SocketHandle TcpListen(std::uint16_t port, std::uint16_t* outBoundPort) noexcept;
    // Accept one pending connection on `listener`. Returns kInvalidSocket when none is pending
    // (non-blocking). Fills the peer's (ip,port) host-order when non-null. The accepted socket is
    // non-blocking.
    SocketHandle TcpAccept(SocketHandle listener, std::uint32_t* fromIp,
                           std::uint16_t* fromPort) noexcept;
    // Begin a NON-BLOCKING connect to (ip,port host-order). Returns a handle immediately (poll
    // readiness with TcpConnectStatus) or kInvalidSocket if the socket could not be created.
    SocketHandle TcpConnect(std::uint32_t ip, std::uint16_t port) noexcept;
    // Non-blocking connect progress: 1 = connected, 0 = still in progress, -1 = failed.
    int TcpConnectStatus(SocketHandle socket) noexcept;
    // Send on a connected stream. Returns bytes sent (may be < size - a partial write on a full send
    // buffer), 0 on would-block, -1 on error/closed.
    std::int64_t TcpSend(SocketHandle socket, const void* data, std::size_t size) noexcept;
    // Receive on a connected stream into `out`. Returns bytes read (>0), 0 on would-block (no data
    // yet), or -1 when the connection is closed or on error.
    std::int64_t TcpRecv(SocketHandle socket, void* out, std::size_t outCap) noexcept;
}

#endif // DRACONIC_FOUNDATION_SYSTEM_BACKEND_H
