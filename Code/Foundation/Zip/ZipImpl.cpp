// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// foundation.zip - implementation: the central directory, inflating through zlib, CRC-32, and
// extraction.

module;
#include "Core/Prelude.h"
#include <zlib.h>

module foundation.zip;

import foundation.core;

using namespace foundation::core;

namespace foundation::zip
{
    namespace
    {
        constexpr u32 kEndOfDirectory = 0x06054b50;
        constexpr u32 kDirectoryEntry = 0x02014b50;
        constexpr u32 kLocalHeader = 0x04034b50;
        constexpr u32 kZip64Locator = 0x07064b50;
        constexpr usize kEndSize = 22;            // the end record without its comment
        constexpr usize kDirectoryEntrySize = 46; // a directory entry without its name and extras
        constexpr usize kLocalHeaderSize = 30;    // a local header without its name and extras

        [[nodiscard]] u16 U16(const byte* p) { return static_cast<u16>(static_cast<u16>(p[0]) | (static_cast<u16>(p[1]) << 8)); }
        [[nodiscard]] u32 U32(const byte* p)
        {
            return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) | (static_cast<u32>(p[2]) << 16) | (static_cast<u32>(p[3]) << 24);
        }

        // Reads `size` bytes at `at`; false when the stream ends first.
        bool ReadAt(IStream& stream, u64 at, usize size, Array<byte>& out)
        {
            out.Resize(size);
            if (stream.Seek(static_cast<i64>(at), SeekOrigin::Begin) < 0)
                return false;
            return size == 0 || stream.Read(out.Data(), size) == size;
        }

    }

    u32 Crc32(Span<const byte> bytes, u32 crc)
    {
        // zlib's, fed in pieces its 32-bit count holds.
        uLong value = crc;
        usize at = 0;
        while (at < bytes.Size())
        {
            const usize piece = Min<usize>(bytes.Size() - at, 1u << 30);
            value = crc32(value, reinterpret_cast<const Bytef*>(bytes.Data() + at), static_cast<uInt>(piece));
            at += piece;
        }
        return static_cast<u32>(value);
    }

    bool IsSafeEntryName(StringView name)
    {
        if (name.IsEmpty() || name[0] == u8'/' || name[0] == u8'\\')
            return false;
        if (name.Size() >= 2 && name[1] == u8':')
            return false; // a drive letter
        usize start = 0;
        for (usize i = 0; i <= name.Size(); ++i)
        {
            if (i < name.Size() && name[i] != u8'/' && name[i] != u8'\\')
                continue;
            if (name.SubStr(start, i - start) == StringView(u8".."))
                return false;
            start = i + 1;
        }
        return true;
    }

    bool ZipArchive::Open(IStream& stream, String& error)
    {
        m_stream = &stream;
        m_entries.Clear();
        const i64 length = stream.Size();
        if (length < static_cast<i64>(kEndSize))
        {
            error = String(u8"not a zip archive (too short)");
            return false;
        }
        // The end record is the last thing in the file, before a comment of up to 64 KB.
        const usize tail = static_cast<usize>(Min<i64>(length, static_cast<i64>(kEndSize + 0xFFFF)));
        Array<byte> end;
        if (!ReadAt(stream, static_cast<u64>(length) - tail, tail, end))
        {
            error = String(u8"the archive could not be read");
            return false;
        }
        usize at = tail - kEndSize + 1;
        bool found = false;
        while (at-- > 0)
            if (U32(end.Data() + at) == kEndOfDirectory)
            {
                found = true;
                break;
            }
        if (!found)
        {
            error = String(u8"not a zip archive (no end of central directory)");
            return false;
        }
        const byte* record = end.Data() + at;
        const u16 count = U16(record + 10);
        const u32 directorySize = U32(record + 12);
        const u32 directoryAt = U32(record + 16);
        if ((at >= 20 && U32(end.Data() + at - 20) == kZip64Locator) || count == 0xFFFF || directoryAt == 0xFFFFFFFFu)
        {
            error = String(u8"a Zip64 archive (over 4 GB or 65535 entries) is not read");
            return false;
        }
        if (static_cast<u64>(directoryAt) + directorySize > static_cast<u64>(length))
        {
            error = String(u8"the archive is cut short (its directory runs past its end)");
            return false;
        }
        Array<byte> directory;
        if (!ReadAt(stream, directoryAt, directorySize, directory))
        {
            error = String(u8"the archive's directory could not be read");
            return false;
        }
        usize p = 0;
        for (u16 i = 0; i < count; ++i)
        {
            if (p + kDirectoryEntrySize > directory.Size() || U32(directory.Data() + p) != kDirectoryEntry)
            {
                error = String(u8"the archive's directory is damaged");
                return false;
            }
            const byte* e = directory.Data() + p;
            const u16 flags = U16(e + 8);
            const usize nameLength = U16(e + 28);
            const usize extraLength = U16(e + 30);
            const usize commentLength = U16(e + 32);
            if (p + kDirectoryEntrySize + nameLength + extraLength + commentLength > directory.Size())
            {
                error = String(u8"the archive's directory is damaged");
                return false;
            }
            ZipEntry entry;
            entry.method = U16(e + 10);
            entry.crc = U32(e + 16);
            entry.compressedSize = U32(e + 20);
            entry.size = U32(e + 24);
            entry.localHeader = U32(e + 42);
            entry.name = String(StringView(reinterpret_cast<const utf8char*>(e + kDirectoryEntrySize), nameLength));
            entry.directory = !entry.name.IsEmpty() && entry.name[entry.name.Size() - 1] == u8'/';
            if ((flags & 1u) != 0)
            {
                error = Format(u8"{} is encrypted, which is not read", entry.name);
                return false;
            }
            if (entry.compressedSize == 0xFFFFFFFFu || entry.size == 0xFFFFFFFFu || entry.localHeader == 0xFFFFFFFFu)
            {
                error = String(u8"a Zip64 archive (over 4 GB or 65535 entries) is not read");
                return false;
            }
            m_entries.PushBack(Move(entry));
            p += kDirectoryEntrySize + nameLength + extraLength + commentLength;
        }
        return true;
    }

    u64 ZipArchive::TotalSize() const noexcept
    {
        u64 total = 0;
        for (const ZipEntry& e : m_entries)
            total += e.directory ? 0 : e.size;
        return total;
    }

    bool ZipArchive::Read(const ZipEntry& entry, Array<byte>& out, String& error)
    {
        out.Clear();
        if (m_stream == nullptr)
        {
            error = String(u8"no archive is open");
            return false;
        }
        if (entry.method != 0 && entry.method != 8)
        {
            error = Format(u8"{} is compressed by method {}, which is not read (only stored and deflate)", entry.name, entry.method);
            return false;
        }
        // The data follows the local header, whose name and extras may differ from the directory's.
        Array<byte> header;
        if (!ReadAt(*m_stream, entry.localHeader, kLocalHeaderSize, header) || U32(header.Data()) != kLocalHeader)
        {
            error = Format(u8"{}: its local header is missing or damaged", entry.name);
            return false;
        }
        const u64 dataAt = entry.localHeader + kLocalHeaderSize + U16(header.Data() + 26) + U16(header.Data() + 28);
        Array<byte> packed;
        if (!ReadAt(*m_stream, dataAt, static_cast<usize>(entry.compressedSize), packed))
        {
            error = Format(u8"{}: the archive ends before its data does", entry.name);
            return false;
        }
        if (entry.method == 0)
        {
            if (entry.compressedSize != entry.size)
            {
                error = Format(u8"{}: a stored entry whose sizes differ", entry.name);
                return false;
            }
            out = Move(packed);
        }
        else
        {
            // Raw deflate (zip keeps no zlib header), into a buffer of the size the directory gives:
            // it must end the stream exactly there. Sizes fit zlib's 32-bit counts, as Zip64 is
            // refused.
            out.Resize(static_cast<usize>(entry.size));
            z_stream z{};
            z.next_in = reinterpret_cast<Bytef*>(packed.Data());
            z.avail_in = static_cast<uInt>(packed.Size());
            z.next_out = reinterpret_cast<Bytef*>(out.Data());
            z.avail_out = static_cast<uInt>(out.Size());
            const bool started = inflateInit2(&z, -MAX_WBITS) == Z_OK;
            const int result = started ? inflate(&z, Z_FINISH) : Z_MEM_ERROR;
            const bool whole = result == Z_STREAM_END && z.total_out == entry.size;
            if (started)
                (void)inflateEnd(&z);
            if (!whole)
            {
                error = Format(u8"{} does not inflate (the archive is damaged)", entry.name);
                out.Clear();
                return false;
            }
        }
        if (Crc32(out.AsSpan()) != entry.crc)
        {
            error = Format(u8"{} does not match its checksum (the archive is damaged)", entry.name);
            out.Clear();
            return false;
        }
        return true;
    }

    bool ZipArchive::ExtractAll(StringView directory, String& error, const Function<void(u64 done, u64 total)>& progress)
    {
        for (const ZipEntry& entry : m_entries)
            if (!IsSafeEntryName(entry.name.AsView()))
            {
                error = Format(u8"the archive names a file outside its folder ({}): nothing was extracted", entry.name);
                return false;
            }
        if (!CreateDirectories(directory))
        {
            error = Format(u8"{} could not be made", directory);
            return false;
        }
        const u64 total = TotalSize();
        u64 done = 0;
        Array<byte> bytes;
        for (const ZipEntry& entry : m_entries)
        {
            const String path = PathJoin(directory, entry.name.AsView());
            if (entry.directory)
            {
                if (!CreateDirectories(path.AsView()))
                {
                    error = Format(u8"{} could not be made", path);
                    return false;
                }
                continue;
            }
            if (!Read(entry, bytes, error))
                return false;
            (void)CreateDirectories(PathParent(path.AsView()));
            if (!WriteFile(path.AsView(), bytes.AsSpan()).IsOk())
            {
                error = Format(u8"{} could not be written", path);
                return false;
            }
            done += entry.size;
            if (progress)
                progress(done, total);
        }
        return true;
    }
}
