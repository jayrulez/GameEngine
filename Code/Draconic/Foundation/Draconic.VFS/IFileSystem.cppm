// Draconic::VFS - :ifilesystem partition
//
// The filesystem contract: a minimal read interface (IFileSystem) plus optional
// capability interfaces a backend implements only if it can (enumerate, write,
// watch). C++ has no real interfaces and Draconic builds -fno-rtti, so capability
// discovery is done with virtual As*() query methods that return the interface
// pointer or null - the same idiom the RHI uses (see RHI/Commands.cppm). The
// capability interfaces are independent bases (not derived from IFileSystem), so
// there is no diamond; the As*() override returns `this` (a correct compile-time
// upcast).

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.vfs:ifilesystem;

import draconic.foundation;

using namespace draconic::foundation;

export namespace draconic::vfs
{
    class IEnumerableFileSystem;
    class IWritableFileSystem;
    class IWatchableFileSystem;
    class IStatFileSystem;

    // One entry returned by enumeration. `name` is mount-relative (a single path
    // component), not a full path.
    struct DirEntry
    {
        String name;
        bool isDirectory = false;
    };

    // Minimum surface: read bytes addressed by a logical path.
    class IFileSystem
    {
    public:
        virtual ~IFileSystem() = default;

        // Opens a stream for the logical path, or null on failure.
        [[nodiscard]] virtual UniquePtr<IStream> Open(StringView path, FileMode mode) = 0;
        [[nodiscard]] virtual bool Exists(StringView path) = 0;

        // Capability queries - default null; capable backends override to return
        // `this`. Consumers do `if (auto* w = fs.AsWritable())` rather than cast.
        [[nodiscard]] virtual IEnumerableFileSystem* AsEnumerable() noexcept { return nullptr; }
        [[nodiscard]] virtual IWritableFileSystem* AsWritable() noexcept { return nullptr; }
        [[nodiscard]] virtual IWatchableFileSystem* AsWatchable() noexcept { return nullptr; }
        [[nodiscard]] virtual IStatFileSystem* AsStat() noexcept { return nullptr; }
    };

    // Capability: list directory contents.
    class IEnumerableFileSystem
    {
    public:
        virtual ~IEnumerableFileSystem() = default;

        // Appends the immediate children of `folder` ("" = root) to `out`.
        // Returns NotFound if the folder can't be opened.
        [[nodiscard]] virtual Status Enumerate(StringView folder, Array<DirEntry>& out) = 0;
    };

    // Capability: write and delete entries (creating parent dirs as needed).
    class IWritableFileSystem
    {
    public:
        virtual ~IWritableFileSystem() = default;

        [[nodiscard]] virtual Status Save(StringView path, Span<const byte> data) = 0;
        [[nodiscard]] virtual Status Delete(StringView path) = 0;
        /// Rename/move a file OR directory within the mount. Default: unsupported (archive
        /// and read-only backends).
        [[nodiscard]] virtual Status Move(StringView from, StringView to)
        {
            (void)from;
            (void)to;
            return Status{ErrorCode::NotSupported};
        }
        /// Remove an EMPTY directory (callers empty it first - deleting a directory tree is
        /// a policy decision that belongs above the mount). Default: unsupported.
        [[nodiscard]] virtual Status DeleteDirectory(StringView path)
        {
            (void)path;
            return Status{ErrorCode::NotSupported};
        }
    };

    // Size + last-write time of one entry.
    struct FileStatInfo
    {
        u64 size = 0;
        i64 modifiedTime = 0; // seconds since the unix epoch
    };

    // Capability: cheap file metadata (the cook pipeline's file-hash memo key). Backends whose
    // content is immutable (pak) simply don't implement it - consumers fall back to hashing.
    class IStatFileSystem
    {
    public:
        virtual ~IStatFileSystem() = default;

        // False when `path` is not a regular file on this mount.
        [[nodiscard]] virtual bool Stat(StringView path, FileStatInfo& out) = 0;
    };

    // Per-mount notifier for content changes (hot reload). Polled by consumers.
    class IChangeSource
    {
    public:
        virtual ~IChangeSource() = default;

        virtual void Track(StringView locator) = 0;
        virtual void Untrack(StringView locator) = 0;
        // Appends changed locators to `outChanged`; returns true if any changed.
        [[nodiscard]] virtual bool Poll(Array<String>& outChanged) = 0;
    };

    // Capability: expose a change source for hot reload. (Seam - no backend
    // implements this yet; lands with the hot-reload pass.)
    class IWatchableFileSystem
    {
    public:
        virtual ~IWatchableFileSystem() = default;

        [[nodiscard]] virtual IChangeSource* ChangeSource() = 0;
    };
}
