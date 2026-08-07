// Draconic::VFS - :native_filesystem partition
//
// NativeFileSystem: backs logical paths with a real directory prefix. Supports
// read, enumerate, write, stat, and watch.
//
// The watch capability is a STAT-SWEEP change source: Track(dir) snapshots every regular file
// under the directory (recursive; "" = the whole mount); each Poll() re-walks the tracked
// trees and diffs (size, mtime) against the snapshot, emitting added/changed/removed locators.
// Deterministic and portable (no inotify/RDCW plumbing); a sweep is O(files), so CALLERS
// throttle the poll rate (the editor polls every couple of seconds). Platform event backends
// can replace the sweep behind the same Poll() later.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.vfs:native_filesystem;

import draconic.foundation;
import :ifilesystem;

using namespace draconic::foundation;

export namespace draconic::vfs
{
    // =======================================================================
    // NativeFileSystem - backs logical paths with a real directory prefix.
    // =======================================================================
    // Stat-sweep change source over a NativeFileSystem (see the header comment).
    class NativeChangeSource final : public IChangeSource
    {
    public:
        explicit NativeChangeSource(IFileSystem& fs) : m_fs(&fs) {}

        void Track(StringView locator) override
        {
            for (const String& existing : m_roots)
            {
                if (existing.AsView() == locator)
                {
                    return;
                }
            }
            m_roots.PushBack(String(locator));
            // Baseline WITHOUT emitting: pre-existing files are not "changes".
            Sweep(locator, nullptr);
        }

        void Untrack(StringView locator) override
        {
            for (usize i = 0; i < m_roots.Size(); ++i)
            {
                if (m_roots[i].AsView() == locator)
                {
                    m_roots.RemoveAt(i);
                    break;
                }
            }
            RebuildSnapshotFromRoots();
        }

        [[nodiscard]] bool Poll(Array<String>& outChanged) override
        {
            const usize before = outChanged.Size();

            // Mark-and-sweep: files seen this walk are marked; snapshot entries left
            // unmarked were removed since the last poll.
            for (auto& [path, entry] : m_snapshot)
            {
                entry.seen = false;
            }
            for (const String& root : m_roots)
            {
                Sweep(root.AsView(), &outChanged);
            }

            Array<String> removed;
            for (auto& [path, entry] : m_snapshot)
            {
                if (!entry.seen)
                {
                    removed.PushBack(String(path.AsView()));
                }
            }
            for (const String& path : removed)
            {
                m_snapshot.Remove(path);
                outChanged.PushBack(String(path.AsView()));
            }
            return outChanged.Size() != before;
        }

    private:
        struct Entry
        {
            u64 size = 0;
            i64 modifiedTime = 0;
            bool seen = false;
        };

        // Walk `folder` recursively; stat regular files; diff against the snapshot. Null
        // `outChanged` = baseline mode (record without emitting).
        void Sweep(StringView folder, Array<String>* outChanged)
        {
            IEnumerableFileSystem* enumerable = m_fs->AsEnumerable();
            IStatFileSystem* stat = m_fs->AsStat();
            if (enumerable == nullptr || stat == nullptr)
            {
                return;
            }

            Array<DirEntry> entries;
            if (!enumerable->Enumerate(folder, entries).IsOk())
            {
                return;
            }
            for (const DirEntry& entry : entries)
            {
                String path(folder);
                if (!path.IsEmpty())
                {
                    path.PushBack(utf8char('/'));
                }
                path.Append(entry.name.AsView());

                if (entry.isDirectory)
                {
                    Sweep(path.AsView(), outChanged);
                    continue;
                }
                FileStatInfo info;
                if (!stat->Stat(path.AsView(), info))
                {
                    continue;
                }

                Entry* known = m_snapshot.Find(path);
                const bool changed = (known == nullptr) || known->size != info.size ||
                                     known->modifiedTime != info.modifiedTime;
                m_snapshot.InsertOrAssign(path, Entry{info.size, info.modifiedTime, true});
                if (changed && outChanged != nullptr)
                {
                    outChanged->PushBack(Move(path));
                }
            }
        }

        void RebuildSnapshotFromRoots()
        {
            m_snapshot.Clear();
            for (const String& root : m_roots)
            {
                Sweep(root.AsView(), nullptr);
            }
        }

        IFileSystem* m_fs;
        Array<String> m_roots;
        HashMap<String, Entry> m_snapshot;
    };

    class NativeFileSystem final : public IFileSystem,
                                   public IEnumerableFileSystem,
                                   public IWritableFileSystem,
                                   public IStatFileSystem,
                                   public IWatchableFileSystem
    {
    public:
        explicit NativeFileSystem(StringView root, IAllocator& allocator = DefaultAllocator())
            : m_root(root, allocator), m_allocator(&allocator)
        {
        }

        // --- IFileSystem ---
        [[nodiscard]] UniquePtr<IStream> Open(StringView path, FileMode mode) override
        {
            const String full = PathJoin(m_root.AsView(), path, *m_allocator);
            FileStream* stream = m_allocator->New<FileStream>(full.AsView(), mode);
            if (stream == nullptr)
            {
                return UniquePtr<IStream>{};
            }
            if (!stream->IsValid())
            {
                m_allocator->Delete(stream);
                return UniquePtr<IStream>{};
            }
            return UniquePtr<IStream>{stream, *m_allocator};
        }

        [[nodiscard]] bool Exists(StringView path) override
        {
            const String full = PathJoin(m_root.AsView(), path, *m_allocator);
            return FileExists(full.AsView()) || DirectoryExists(full.AsView());
        }

        [[nodiscard]] IEnumerableFileSystem* AsEnumerable() noexcept override { return this; }
        [[nodiscard]] IWritableFileSystem* AsWritable() noexcept override { return this; }
        [[nodiscard]] IStatFileSystem* AsStat() noexcept override { return this; }
        [[nodiscard]] IWatchableFileSystem* AsWatchable() noexcept override { return this; }

        // --- IWatchableFileSystem ---
        [[nodiscard]] IChangeSource* ChangeSource() override
        {
            if (!m_changeSource)
            {
                m_changeSource = MakeUnique<NativeChangeSource>(DefaultAllocator(), *this);
            }
            return m_changeSource.Get();
        }

        // --- IStatFileSystem ---
        [[nodiscard]] bool Stat(StringView path, FileStatInfo& out) override
        {
            const String full = PathJoin(m_root.AsView(), path, *m_allocator);
            return FileStat(full.AsView(), out.size, out.modifiedTime);
        }

        // --- IEnumerableFileSystem ---
        [[nodiscard]] Status Enumerate(StringView folder, Array<DirEntry>& out) override
        {
            const String full = PathJoin(m_root.AsView(), folder, *m_allocator);
            const bool ok = ListDirectory(
                full.AsView(),
                [](void* ctx, StringView name, bool isDir)
                {
                    auto* dst = static_cast<Array<DirEntry>*>(ctx);
                    dst->PushBack(DirEntry{String(name), isDir});
                },
                &out);
            return ok ? Status{} : Status{ErrorCode::NotFound};
        }

        // --- IWritableFileSystem ---
        [[nodiscard]] Status Save(StringView path, Span<const byte> data) override
        {
            const String full = PathJoin(m_root.AsView(), path, *m_allocator);
            EnsureParentDirectories(full.AsView());

            FileStream stream(full.AsView(), FileMode::Write);
            if (!stream.IsValid())
            {
                return Status{ErrorCode::Internal};
            }
            if (!data.IsEmpty() && stream.Write(data.Data(), data.Size()) != data.Size())
            {
                return Status{ErrorCode::Internal};
            }
            return Status{};
        }

        [[nodiscard]] Status Delete(StringView path) override
        {
            const String full = PathJoin(m_root.AsView(), path, *m_allocator);
            return FileDelete(full.AsView()) ? Status{} : Status{ErrorCode::NotFound};
        }

        [[nodiscard]] Status Move(StringView from, StringView to) override
        {
            const String fullFrom = PathJoin(m_root.AsView(), from, *m_allocator);
            const String fullTo = PathJoin(m_root.AsView(), to, *m_allocator);
            EnsureParentDirectories(fullTo.AsView());
            return FileMove(fullFrom.AsView(), fullTo.AsView()) ? Status{}
                                                                : Status{ErrorCode::NotFound};
        }

        [[nodiscard]] Status DeleteDirectory(StringView path) override
        {
            const String full = PathJoin(m_root.AsView(), path, *m_allocator);
            return RemoveDirectory(full.AsView()) ? Status{} : Status{ErrorCode::NotFound};
        }

    private:
        // Creates every ancestor directory of `full` (idempotent). The final
        // component is the file itself and is left to the caller.
        static void EnsureParentDirectories(StringView full)
        {
            for (usize i = 1; i < full.Size(); ++i)
            {
                if (full[i] == utf8char('/') || full[i] == utf8char('\\'))
                {
                    (void)CreateDirectory(full.SubStr(0, i));
                }
            }
        }

        String m_root;
        IAllocator* m_allocator;
        UniquePtr<NativeChangeSource> m_changeSource; // lazy (most mounts never watch)
    };
}
