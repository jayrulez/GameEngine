// Draconic::VFS::Pak - the `draconic.vfs.pak` module.
//
// A packed, read-only archive backend for the VFS (the shipping counterpart to
// the disk backend). PakFileSystem implements read + enumerate over a single
// .pak file; it is immutable at runtime (no write/watch). PakBuilder is the
// offline writer. Lives in its own module so future compression codecs never
// pull into the core VFS.
//
// File format (little-endian as stored):
//   [Header, 32 bytes]
//     u32 magic   = 'RPAK'
//     u32 version = 1
//     u64 entryCount
//     u64 tocOffset   bytes from start of file
//     u64 tocSize     bytes
//   [Data heap] entry bytes packed back-to-back (stored form)
//   [TOC at tocOffset] per entry:
//     u16 locatorLength (UTF-8 bytes)  | u8[] locator
//     u64 offset | u64 storedSize | u64 originalSize | u16 compression

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.vfs.pak;

import draconic.foundation;
import draconic.vfs;

using namespace draconic::foundation;

export namespace draconic::vfs
{
    inline constexpr u32 kPakMagic = 0x4B415052u; // 'RPAK'
    inline constexpr u32 kPakVersion = 1u;
    inline constexpr u16 kCompressionNone = 0u;

    // =======================================================================
    // PakFileSystem - read + enumerate over a .pak archive. Immutable.
    // =======================================================================
    class PakFileSystem final : public IFileSystem, public IEnumerableFileSystem
    {
    public:
        explicit PakFileSystem(StringView pakPath, IAllocator& allocator = DefaultAllocator())
            : m_path(pakPath, allocator), m_allocator(&allocator)
        {
            Load();
        }

        // True if the archive opened and parsed.
        [[nodiscard]] bool IsValid() const noexcept { return m_valid; }
        [[nodiscard]] usize EntryCount() const noexcept { return m_entries.Size(); }

        // --- IFileSystem ---
        [[nodiscard]] UniquePtr<IStream> Open(StringView locator, FileMode mode) override
        {
            if (!m_valid || mode != FileMode::Read)
            {
                return UniquePtr<IStream>{};
            }

            const Entry* entry = Find(locator);
            if (entry == nullptr || entry->compression != kCompressionNone)
            {
                return UniquePtr<IStream>{};
            }

            // Reopen the archive per Open: a fresh handle, no shared seek lock.
            FileStream file(m_path.AsView(), FileMode::Read);
            if (!file.IsValid())
            {
                return UniquePtr<IStream>{};
            }
            if (file.Seek(static_cast<i64>(entry->offset), SeekOrigin::Begin) < 0)
            {
                return UniquePtr<IStream>{};
            }

            Array<byte> bytes(static_cast<usize>(entry->storedSize), *m_allocator);
            if (entry->storedSize > 0 &&
                file.Read(bytes.Data(), entry->storedSize) != entry->storedSize)
            {
                return UniquePtr<IStream>{};
            }

            // Hand the bytes to an in-memory stream the caller owns.
            MemoryStream* stream = m_allocator->New<MemoryStream>();
            if (entry->storedSize > 0)
            {
                (void)stream->Write(bytes.Data(), entry->storedSize);
            }
            (void)stream->Seek(0, SeekOrigin::Begin);
            return UniquePtr<IStream>{stream, *m_allocator};
        }

        [[nodiscard]] bool Exists(StringView locator) override
        {
            return m_valid && Find(locator) != nullptr;
        }

        [[nodiscard]] IEnumerableFileSystem* AsEnumerable() noexcept override { return this; }

        // --- IEnumerableFileSystem ---
        [[nodiscard]] Status Enumerate(StringView folder, Array<DirEntry>& out) override
        {
            if (!m_valid)
            {
                return Status{ErrorCode::NotFound};
            }

            for (const Entry& entry : m_entries)
            {
                StringView rel;
                if (!RelativeUnder(entry.locator.AsView(), folder, rel))
                {
                    continue;
                }

                // First path segment of `rel`: a '/' means it's a subdirectory.
                usize slash = rel.Size();
                for (usize i = 0; i < rel.Size(); ++i)
                {
                    if (rel[i] == utf8char('/'))
                    {
                        slash = i;
                        break;
                    }
                }

                if (slash == rel.Size())
                {
                    out.PushBack(DirEntry{String(rel), false}); // a file
                }
                else
                {
                    const StringView dir = rel.SubStr(0, slash);
                    if (!ContainsDir(out, dir))
                    {
                        out.PushBack(DirEntry{String(dir), true});
                    }
                }
            }
            return Status{};
        }

    private:
        struct Entry
        {
            String locator;
            u64 offset = 0;
            u64 storedSize = 0;
            u64 originalSize = 0;
            u16 compression = kCompressionNone;
        };

        void Load()
        {
            FileStream file(m_path.AsView(), FileMode::Read);
            if (!file.IsValid())
            {
                return;
            }

            BinaryReader reader(file);
            u32 magic = 0;
            u32 version = 0;
            u64 entryCount = 0;
            u64 tocOffset = 0;
            u64 tocSize = 0;
            reader.Read(magic);
            reader.Read(version);
            reader.Read(entryCount);
            reader.Read(tocOffset);
            reader.Read(tocSize);
            if (!reader.IsOk() || magic != kPakMagic || version != kPakVersion)
            {
                return;
            }

            if (file.Seek(static_cast<i64>(tocOffset), SeekOrigin::Begin) < 0)
            {
                return;
            }
            for (u64 i = 0; i < entryCount; ++i)
            {
                u16 locatorLength = 0;
                reader.Read(locatorLength);
                Array<byte> locatorBytes(static_cast<usize>(locatorLength), *m_allocator);
                if (locatorLength > 0 && !reader.ReadBytes(locatorBytes.Data(), locatorLength))
                {
                    return;
                }

                Entry entry;
                // Locator is stored as UTF-8 on disk; String is already UTF-8.
                entry.locator = String(StringView(
                    reinterpret_cast<const utf8char*>(locatorBytes.Data()), locatorLength));
                reader.Read(entry.offset);
                reader.Read(entry.storedSize);
                reader.Read(entry.originalSize);
                reader.Read(entry.compression);
                if (!reader.IsOk())
                {
                    return;
                }
                m_entries.PushBack(static_cast<Entry&&>(entry));
            }
            m_valid = true;
        }

        [[nodiscard]] const Entry* Find(StringView locator) const
        {
            for (const Entry& entry : m_entries)
            {
                if (entry.locator.AsView() == locator)
                {
                    return &entry;
                }
            }
            return nullptr;
        }

        // Is `locator` under `folder`? If so, `outRel` is the remainder.
        [[nodiscard]] static bool RelativeUnder(StringView locator, StringView folder,
                                                StringView& outRel)
        {
            if (folder.IsEmpty())
            {
                outRel = locator;
                return true;
            }
            if (locator.Size() <= folder.Size() + 1)
            {
                return false;
            }
            if (locator.SubStr(0, folder.Size()) != folder)
            {
                return false;
            }
            if (locator[folder.Size()] != utf8char('/'))
            {
                return false;
            }
            outRel = locator.SubStr(folder.Size() + 1, locator.Size() - folder.Size() - 1);
            return true;
        }

        [[nodiscard]] static bool ContainsDir(const Array<DirEntry>& out, StringView name)
        {
            for (const DirEntry& entry : out)
            {
                if (entry.isDirectory && entry.name.AsView() == name)
                {
                    return true;
                }
            }
            return false;
        }

        String m_path;
        IAllocator* m_allocator;
        Array<Entry> m_entries;
        bool m_valid = false;
    };

    // =======================================================================
    // PakBuilder - offline writer. Add entries, then Write the archive.
    // =======================================================================
    class PakBuilder
    {
    public:
        // Copies `data` immediately; the caller may free its buffer afterwards.
        void Add(StringView locator, Span<const byte> data)
        {
            PendingEntry entry;
            entry.locator = String(locator);
            entry.data.Resize(data.Size());
            if (data.Size() > 0)
            {
                MemCopy(entry.data.Data(), data.Data(), data.Size());
            }
            m_entries.PushBack(static_cast<PendingEntry&&>(entry));
        }

        [[nodiscard]] Status Write(StringView path) const
        {
            MemoryStream out;
            BinaryWriter writer(out);

            const u32 magic = kPakMagic;
            const u32 version = kPakVersion;
            u64 entryCount = m_entries.Size();
            u64 tocOffset = 0;
            u64 tocSize = 0;

            // Header placeholder (patched once offsets are known).
            WriteHeader(writer, magic, version, entryCount, tocOffset, tocSize);

            // Data heap.
            Array<u64> offsets;
            for (const PendingEntry& entry : m_entries)
            {
                offsets.PushBack(static_cast<u64>(out.Tell()));
                if (!entry.data.IsEmpty())
                {
                    writer.WriteBytes(entry.data.Data(), entry.data.Size());
                }
            }

            // TOC.
            tocOffset = static_cast<u64>(out.Tell());
            for (usize i = 0; i < m_entries.Size(); ++i)
            {
                // Locator is already UTF-8; write directly.
                const String& locatorStr = m_entries[i].locator;
                const u16 locatorLength = static_cast<u16>(locatorStr.Size());
                writer.Write(locatorLength);
                if (locatorLength > 0)
                {
                    writer.WriteBytes(locatorStr.CStr(), locatorStr.Size());
                }

                const u64 size = m_entries[i].data.Size();
                const u16 compression = kCompressionNone;
                writer.Write(offsets[i]);
                writer.Write(size); // storedSize
                writer.Write(size); // originalSize
                writer.Write(compression);
            }
            tocSize = static_cast<u64>(out.Tell()) - tocOffset;

            // Patch the header in place.
            (void)out.Seek(0, SeekOrigin::Begin);
            WriteHeader(writer, magic, version, entryCount, tocOffset, tocSize);

            if (!writer.IsOk())
            {
                return Status{ErrorCode::Internal};
            }
            return WriteFile(path, out.Bytes());
        }

    private:
        struct PendingEntry
        {
            String locator;
            Array<byte> data;
        };

        static void WriteHeader(BinaryWriter& writer, u32 magic, u32 version, u64 entryCount,
                                u64 tocOffset, u64 tocSize)
        {
            writer.Write(magic);
            writer.Write(version);
            writer.Write(entryCount);
            writer.Write(tocOffset);
            writer.Write(tocSize);
        }

        Array<PendingEntry> m_entries;
    };
}
