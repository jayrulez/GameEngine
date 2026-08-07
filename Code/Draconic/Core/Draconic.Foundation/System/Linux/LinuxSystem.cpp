// Draconic Foundation - System backend, Linux implementation.

#include "Draconic.Foundation/System/SystemBackend.h"

#include <ctime>
#include <cerrno>
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <cstdio>  // std::rename
#include <cstdlib> // std::getenv
#include <cstring> // std::strlen, std::memcpy

namespace draconic::foundation::sys
{
    std::uint64_t GetTicks() noexcept
    {
        timespec ts{};
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return static_cast<std::uint64_t>(ts.tv_sec) * 1'000'000'000ull +
               static_cast<std::uint64_t>(ts.tv_nsec);
    }

    std::uint64_t GetTickFrequency() noexcept
    {
        return 1'000'000'000ull; // GetTicks() is in nanoseconds
    }

    void SleepMilliseconds(std::uint32_t milliseconds) noexcept
    {
        timespec req{};
        req.tv_sec = static_cast<time_t>(milliseconds / 1000u);
        req.tv_nsec = static_cast<long>((milliseconds % 1000u) * 1'000'000ull);

        timespec rem{};
        while (nanosleep(&req, &rem) == -1)
        {
            req = rem; // interrupted by a signal - resume for the remainder
        }
    }

    std::uint32_t LogicalCoreCount() noexcept
    {
        const long count = sysconf(_SC_NPROCESSORS_ONLN);
        return (count > 0) ? static_cast<std::uint32_t>(count) : 1u;
    }

    std::size_t PageSize() noexcept
    {
        const long size = sysconf(_SC_PAGESIZE);
        return (size > 0) ? static_cast<std::size_t>(size) : 4096u;
    }

    std::size_t GetEnvironmentVariable(const char* name, char* out, std::size_t outSize) noexcept
    {
        const char* value = std::getenv(name);
        if (value == nullptr)
        {
            return 0;
        }
        const std::size_t length = std::strlen(value);
        if (out != nullptr && outSize > 0)
        {
            const std::size_t n = (length < outSize - 1) ? length : outSize - 1;
            std::memcpy(out, value, n);
            out[n] = '\0';
        }
        return length;
    }

    std::size_t GetUserDataDirectory(char* out, std::size_t outSize) noexcept
    {
        // XDG spec: $XDG_DATA_HOME, else ~/.local/share. snprintf returns the length it WOULD write
        // (excl null), matching the truncation contract; it also tolerates out == nullptr.
        if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg != nullptr && xdg[0] != '\0')
        {
            const int n = std::snprintf(out, outSize, "%s", xdg);
            return (n > 0) ? static_cast<std::size_t>(n) : 0;
        }
        if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0')
        {
            const int n = std::snprintf(out, outSize, "%s/.local/share", home);
            return (n > 0) ? static_cast<std::size_t>(n) : 0;
        }
        return 0;
    }

    const char* GetHostPlatformName() noexcept { return "Linux64"; }
    const char* ExecutableExtension() noexcept { return ""; }

    std::size_t GetExecutablePath(char* out, std::size_t outSize) noexcept
    {
        char buffer[4096];
        const ssize_t n = readlink("/proc/self/exe", buffer, sizeof(buffer));
        if (n <= 0)
        {
            return 0;
        }
        const std::size_t length = static_cast<std::size_t>(n);
        if (out != nullptr && outSize > 0)
        {
            const std::size_t k = (length < outSize - 1) ? length : outSize - 1;
            std::memcpy(out, buffer, k);
            out[k] = '\0';
        }
        return length;
    }

    bool OpenPathInFileManager(const char* path) noexcept
    {
        if (path == nullptr || path[0] == '\0')
        {
            return false;
        }
        // Double-fork so the grandchild (xdg-open) reparents to init and is reaped there - no zombie,
        // no global SIGCHLD change. The parent only waits on the intermediate child, which exits
        // immediately, so this never blocks the UI thread. xdg-open detaches from our stdio via setsid.
        const pid_t child = fork();
        if (child < 0)
        {
            return false;
        }
        if (child == 0)
        {
            const pid_t grandchild = fork();
            if (grandchild == 0)
            {
                setsid();
                execlp("xdg-open", "xdg-open", path, static_cast<char*>(nullptr));
                _exit(127); // exec failed
            }
            _exit(0); // intermediate exits right away
        }
        int status = 0;
        (void)waitpid(child, &status, 0);
        return true; // launch initiated (xdg-open's own success isn't observable here)
    }

    int RunProcess(const char* exe, const char* const* argv, int argc, char* out,
                   std::size_t outCap) noexcept
    {
        if (exe == nullptr || exe[0] == '\0')
        {
            return -1;
        }
        const bool capture = (out != nullptr && outCap > 0);
        if (capture)
        {
            out[0] = '\0';
        }

        // Build argv2 = [exe, argv..., nullptr] BEFORE the fork so the child does no allocation.
        constexpr int kMaxArgs = 62;
        const char* argv2[kMaxArgs + 2];
        int n = 0;
        argv2[n++] = exe;
        for (int i = 0; i < argc && n <= kMaxArgs; ++i)
        {
            argv2[n++] = argv[i];
        }
        argv2[n] = nullptr;

        int pipefd[2] = {-1, -1};
        if (capture && pipe(pipefd) != 0)
        {
            return -1;
        }
        // Self-pipe (close-on-exec) so the child can report an exec() failure to the parent: on a
        // successful exec the write end auto-closes and the parent reads EOF; on failure the child
        // writes errno first. Without this a missing binary would surface as the child's exit 127,
        // indistinguishable from a real 127, and inconsistent with the Win32 spawn-fails -> -1 path.
        int execErr[2] = {-1, -1};
        if (pipe(execErr) != 0)
        {
            if (capture)
            {
                close(pipefd[0]);
                close(pipefd[1]);
            }
            return -1;
        }
        fcntl(execErr[1], F_SETFD, FD_CLOEXEC);

        const pid_t child = fork();
        if (child < 0)
        {
            if (capture)
            {
                close(pipefd[0]);
                close(pipefd[1]);
            }
            close(execErr[0]);
            close(execErr[1]);
            return -1;
        }
        if (child == 0)
        {
            // Child: stdout+stderr -> pipe, stdin <- /dev/null, then exec.
            if (capture)
            {
                dup2(pipefd[1], STDOUT_FILENO);
                dup2(pipefd[1], STDERR_FILENO);
                close(pipefd[0]);
                close(pipefd[1]);
            }
            close(execErr[0]);
            const int devnull = ::open("/dev/null", O_RDONLY);
            if (devnull >= 0)
            {
                dup2(devnull, STDIN_FILENO);
                close(devnull);
            }
            execv(exe, const_cast<char* const*>(argv2));
            const int e = errno; // exec failed - report to the parent, then bail
            const ssize_t wrote = write(execErr[1], &e, sizeof(e));
            (void)wrote;
            _exit(127);
        }

        // Parent: first learn whether exec succeeded (read blocks until data or EOF-on-exec).
        close(execErr[1]);
        int childErr = 0;
        const ssize_t got = read(execErr[0], &childErr, sizeof(childErr));
        close(execErr[0]);
        const bool execFailed = (got > 0);

        // Drain stdout+stderr (keep reading past a full buffer so the child never blocks), then reap.
        std::size_t total = 0;
        if (capture)
        {
            close(pipefd[1]);
            for (;;)
            {
                char buffer[4096];
                const ssize_t r = read(pipefd[0], buffer, sizeof(buffer));
                if (r <= 0)
                {
                    break;
                }
                if (total < outCap - 1)
                {
                    const std::size_t space = outCap - 1 - total;
                    const std::size_t k = (static_cast<std::size_t>(r) < space)
                                              ? static_cast<std::size_t>(r)
                                              : space;
                    std::memcpy(out + total, buffer, k);
                    total += k;
                }
            }
            out[total] = '\0';
            close(pipefd[0]);
        }
        int status = 0;
        if (waitpid(child, &status, 0) < 0)
        {
            return -1;
        }
        if (execFailed)
        {
            return -1; // the target program never ran
        }
        return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }

    std::size_t GetCurrentDirectory(char* out, std::size_t outSize) noexcept
    {
        char buffer[4096];
        if (getcwd(buffer, sizeof(buffer)) == nullptr)
        {
            return 0;
        }
        const std::size_t length = std::strlen(buffer);
        if (out != nullptr && outSize > 0)
        {
            const std::size_t k = (length < outSize - 1) ? length : outSize - 1;
            std::memcpy(out, buffer, k);
            out[k] = '\0';
        }
        return length;
    }

    void* PageAllocate(std::size_t size) noexcept
    {
        if (size == 0)
        {
            return nullptr;
        }

        void* memory =
            mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        return (memory == MAP_FAILED) ? nullptr : memory;
    }

    void PageFree(void* pointer, std::size_t size) noexcept
    {
        if (pointer != nullptr && size != 0)
        {
            munmap(pointer, size);
        }
    }

    FileHandle FileOpen(const char* path, FileMode mode) noexcept
    {
        int flags = 0;
        switch (mode)
        {
        case FileMode::Read:
            flags = O_RDONLY;
            break;
        case FileMode::Write:
            flags = O_WRONLY | O_CREAT | O_TRUNC;
            break;
        case FileMode::ReadWrite:
            flags = O_RDWR | O_CREAT;
            break;
        case FileMode::Append:
            flags = O_WRONLY | O_CREAT | O_APPEND;
            break;
        }

        const int fd = open(path, flags, 0644);
        return (fd < 0) ? kInvalidFile : static_cast<FileHandle>(fd);
    }

    void FileClose(FileHandle handle) noexcept
    {
        if (handle != kInvalidFile)
        {
            close(static_cast<int>(handle));
        }
    }

    std::int64_t FileRead(FileHandle handle, void* buffer, std::uint64_t bytes) noexcept
    {
        const ssize_t n = read(static_cast<int>(handle), buffer, static_cast<std::size_t>(bytes));
        return static_cast<std::int64_t>(n);
    }

    std::int64_t FileWrite(FileHandle handle, const void* buffer, std::uint64_t bytes) noexcept
    {
        const ssize_t n = write(static_cast<int>(handle), buffer, static_cast<std::size_t>(bytes));
        return static_cast<std::int64_t>(n);
    }

    std::int64_t FileSeek(FileHandle handle, std::int64_t offset, SeekOrigin origin) noexcept
    {
        int whence = SEEK_SET;
        switch (origin)
        {
        case SeekOrigin::Begin:
            whence = SEEK_SET;
            break;
        case SeekOrigin::Current:
            whence = SEEK_CUR;
            break;
        case SeekOrigin::End:
            whence = SEEK_END;
            break;
        }

        const off_t pos = lseek(static_cast<int>(handle), static_cast<off_t>(offset), whence);
        return static_cast<std::int64_t>(pos);
    }

    std::int64_t FileSize(FileHandle handle) noexcept
    {
        struct stat st{};
        if (fstat(static_cast<int>(handle), &st) != 0)
        {
            return -1;
        }
        return static_cast<std::int64_t>(st.st_size);
    }

    bool FileExists(const char* path) noexcept { return access(path, F_OK) == 0; }

    bool FileStat(const char* path, unsigned long long& outSize,
                  long long& outModifiedTime) noexcept
    {
        struct stat st{};
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
        {
            return false;
        }
        outSize = static_cast<unsigned long long>(st.st_size);
        outModifiedTime = static_cast<long long>(st.st_mtime);
        return true;
    }

    bool FileDelete(const char* path) noexcept { return unlink(path) == 0; }

    bool FileMove(const char* from, const char* to) noexcept { return std::rename(from, to) == 0; }

    bool FileCopyPreserving(const char* from, const char* to) noexcept
    {
        const int src = ::open(from, O_RDONLY);
        if (src < 0)
        {
            return false;
        }
        struct stat st{};
        if (::fstat(src, &st) != 0)
        {
            ::close(src);
            return false;
        }
        const int dst = ::open(to, O_WRONLY | O_CREAT | O_TRUNC, st.st_mode & 07777);
        if (dst < 0)
        {
            ::close(src);
            return false;
        }
        bool ok = true;
        char buffer[64 * 1024];
        for (;;)
        {
            const ssize_t got = ::read(src, buffer, sizeof(buffer));
            if (got == 0)
            {
                break;
            }
            if (got < 0)
            {
                ok = false;
                break;
            }
            ssize_t written = 0;
            while (written < got)
            {
                const ssize_t put =
                    ::write(dst, buffer + written, static_cast<size_t>(got - written));
                if (put <= 0)
                {
                    ok = false;
                    break;
                }
                written += put;
            }
            if (!ok)
            {
                break;
            }
        }
        // O_CREAT mode is masked by umask - re-apply the exact source mode (the +x bit).
        if (ok && ::fchmod(dst, st.st_mode & 07777) != 0)
        {
            ok = false;
        }
        ::close(src);
        ::close(dst);
        return ok;
    }

    bool DirectoryExists(const char* path) noexcept
    {
        struct stat st{};
        return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
    }

    bool CreateDirectory(const char* path) noexcept
    {
        if (mkdir(path, 0755) == 0)
        {
            return true;
        }
        return DirectoryExists(path); // already exists is success
    }

    bool RemoveDirectory(const char* path) noexcept { return rmdir(path) == 0; }

    bool ListDirectory(const char* path, DirEntryCallback cb, void* ctx) noexcept
    {
        DIR* dir = opendir(path);
        if (dir == nullptr)
        {
            return false;
        }

        for (struct dirent* entry = readdir(dir); entry != nullptr; entry = readdir(dir))
        {
            const char* name = entry->d_name;
            if (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0')))
            {
                continue; // skip "." and ".."
            }

            bool isDir = false;
            if (entry->d_type == DT_DIR)
            {
                isDir = true;
            }
            else if (entry->d_type == DT_UNKNOWN) // some filesystems don't fill d_type
            {
                struct stat st{};
                if (fstatat(dirfd(dir), name, &st, 0) == 0)
                {
                    isDir = S_ISDIR(st.st_mode);
                }
            }
            cb(ctx, name, isDir);
        }

        closedir(dir);
        return true;
    }

    void ConsoleWrite(const char* text, std::uint64_t length) noexcept
    {
        ssize_t result = write(STDOUT_FILENO, text, static_cast<std::size_t>(length));
        (void)result;
    }

    void ConsoleWriteError(const char* text, std::uint64_t length) noexcept
    {
        ssize_t result = write(STDERR_FILENO, text, static_cast<std::size_t>(length));
        (void)result;
    }

    LibraryHandle LibraryOpen(const char* path) noexcept
    {
        return dlopen(path, RTLD_NOW | RTLD_LOCAL);
    }

    void* LibrarySymbol(LibraryHandle handle, const char* name) noexcept
    {
        return (handle != nullptr) ? dlsym(handle, name) : nullptr;
    }

    void LibraryClose(LibraryHandle handle) noexcept
    {
        if (handle != nullptr)
        {
            dlclose(handle);
        }
    }

    // --- UDP sockets -------------------------------------------------------

    bool InitializeNetworking() noexcept { return true; } // POSIX needs no init
    void ShutdownNetworking() noexcept {}

    SocketHandle UdpOpen(std::uint16_t port, std::uint16_t* outBoundPort) noexcept
    {
        const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (fd < 0)
        {
            return kInvalidSocket;
        }
        const int flags = ::fcntl(fd, F_GETFL, 0);
        ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        const int yes = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(port);
        if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
        {
            ::close(fd);
            return kInvalidSocket;
        }
        if (outBoundPort != nullptr)
        {
            sockaddr_in bound{};
            socklen_t len = sizeof(bound);
            *outBoundPort = (::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &len) == 0)
                                ? ntohs(bound.sin_port)
                                : port;
        }
        return static_cast<SocketHandle>(fd);
    }

    void SocketClose(SocketHandle socket) noexcept
    {
        if (socket != kInvalidSocket)
        {
            ::close(static_cast<int>(socket));
        }
    }

    bool ParseIPv4(const char* dottedQuad, std::uint32_t* outIp) noexcept
    {
        in_addr a{};
        if (::inet_pton(AF_INET, dottedQuad, &a) != 1)
        {
            return false;
        }
        if (outIp != nullptr)
        {
            *outIp = ntohl(a.s_addr);
        }
        return true;
    }

    std::int64_t UdpSendTo(SocketHandle socket, std::uint32_t ip, std::uint16_t port,
                           const void* data, std::size_t size) noexcept
    {
        if (socket == kInvalidSocket)
        {
            return -1;
        }
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(ip);
        addr.sin_port = htons(port);
        const ssize_t n = ::sendto(static_cast<int>(socket), data, size, 0,
                                   reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        if (n < 0)
        {
            return (errno == EWOULDBLOCK || errno == EAGAIN) ? 0 : -1;
        }
        return static_cast<std::int64_t>(n);
    }

    std::int64_t UdpRecvFrom(SocketHandle socket, void* out, std::size_t outCap,
                             std::uint32_t* fromIp, std::uint16_t* fromPort) noexcept
    {
        if (socket == kInvalidSocket)
        {
            return -1;
        }
        sockaddr_in addr{};
        socklen_t len = sizeof(addr);
        const ssize_t n = ::recvfrom(static_cast<int>(socket), out, outCap, 0,
                                     reinterpret_cast<sockaddr*>(&addr), &len);
        if (n < 0)
        {
            return (errno == EWOULDBLOCK || errno == EAGAIN) ? 0 : -1;
        }
        if (fromIp != nullptr)
        {
            *fromIp = ntohl(addr.sin_addr.s_addr);
        }
        if (fromPort != nullptr)
        {
            *fromPort = ntohs(addr.sin_port);
        }
        return static_cast<std::int64_t>(n);
    }

    // --- TCP sockets -------------------------------------------------------

    static void SetNonBlocking(int fd) noexcept
    {
        const int flags = ::fcntl(fd, F_GETFL, 0);
        ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }

    SocketHandle TcpListen(std::uint16_t port, std::uint16_t* outBoundPort) noexcept
    {
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0)
        {
            return kInvalidSocket;
        }
        SetNonBlocking(fd);
        const int yes = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(port);
        if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
        {
            ::close(fd);
            return kInvalidSocket;
        }
        if (::listen(fd, 16) < 0)
        {
            ::close(fd);
            return kInvalidSocket;
        }
        if (outBoundPort != nullptr)
        {
            sockaddr_in bound{};
            socklen_t len = sizeof(bound);
            *outBoundPort = (::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &len) == 0)
                                ? ntohs(bound.sin_port)
                                : port;
        }
        return static_cast<SocketHandle>(fd);
    }

    SocketHandle TcpAccept(SocketHandle listener, std::uint32_t* fromIp,
                           std::uint16_t* fromPort) noexcept
    {
        if (listener == kInvalidSocket)
        {
            return kInvalidSocket;
        }
        sockaddr_in addr{};
        socklen_t len = sizeof(addr);
        const int fd =
            ::accept(static_cast<int>(listener), reinterpret_cast<sockaddr*>(&addr), &len);
        if (fd < 0)
        {
            return kInvalidSocket;
        } // EWOULDBLOCK => none pending
        SetNonBlocking(fd);
        if (fromIp != nullptr)
        {
            *fromIp = ntohl(addr.sin_addr.s_addr);
        }
        if (fromPort != nullptr)
        {
            *fromPort = ntohs(addr.sin_port);
        }
        return static_cast<SocketHandle>(fd);
    }

    SocketHandle TcpConnect(std::uint32_t ip, std::uint16_t port) noexcept
    {
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0)
        {
            return kInvalidSocket;
        }
        SetNonBlocking(fd);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(ip);
        addr.sin_port = htons(port);
        const int r = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        if (r < 0 && errno != EINPROGRESS)
        {
            ::close(fd);
            return kInvalidSocket;
        }
        return static_cast<SocketHandle>(fd);
    }

    int TcpConnectStatus(SocketHandle socket) noexcept
    {
        if (socket == kInvalidSocket)
        {
            return -1;
        }
        pollfd pfd{};
        pfd.fd = static_cast<int>(socket);
        pfd.events = POLLOUT;
        const int r = ::poll(&pfd, 1, 0);
        if (r == 0)
        {
            return 0;
        } // still connecting
        if (r < 0)
        {
            return -1;
        }
        if ((pfd.revents & (POLLERR | POLLHUP)) != 0)
        {
            return -1;
        }
        if ((pfd.revents & POLLOUT) != 0)
        {
            int err = 0;
            socklen_t len = sizeof(err);
            if (::getsockopt(static_cast<int>(socket), SOL_SOCKET, SO_ERROR, &err, &len) < 0)
            {
                return -1;
            }
            return (err == 0) ? 1 : -1;
        }
        return 0;
    }

    std::int64_t TcpSend(SocketHandle socket, const void* data, std::size_t size) noexcept
    {
        if (socket == kInvalidSocket)
        {
            return -1;
        }
        const ssize_t n = ::send(static_cast<int>(socket), data, size, MSG_NOSIGNAL);
        if (n < 0)
        {
            return (errno == EWOULDBLOCK || errno == EAGAIN) ? 0 : -1;
        }
        return static_cast<std::int64_t>(n);
    }

    std::int64_t TcpRecv(SocketHandle socket, void* out, std::size_t outCap) noexcept
    {
        if (socket == kInvalidSocket)
        {
            return -1;
        }
        const ssize_t n = ::recv(static_cast<int>(socket), out, outCap, 0);
        if (n > 0)
        {
            return static_cast<std::int64_t>(n);
        }
        if (n == 0)
        {
            return -1;
        } // peer closed the connection
        return (errno == EWOULDBLOCK || errno == EAGAIN) ? 0 : -1;
    }
}

// Emscripten reuses this POSIX backend but has no execinfo - wasm stack traces come from
// the host (node --stack-trace-limit / browser devtools), so WriteBacktrace is a no-op.
#if defined(__EMSCRIPTEN__)

namespace draconic::foundation::sys
{
    int WriteBacktrace(int) noexcept { return 0; }
}

#else

#include <execinfo.h>

namespace draconic::foundation::sys
{
    int WriteBacktrace(int fd) noexcept
    {
        void* frames[64];
        const int count = backtrace(frames, 64);
        backtrace_symbols_fd(frames, count, fd);
        return count;
    }
}

#endif
