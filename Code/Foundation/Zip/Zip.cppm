// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Zip - `foundation.zip`: reading a zip archive (PKWARE's APPNOTE format, as zip
// tools and GitHub release assets write it), for the sample downloads.
//
// The central directory is read from the end of the stream; each entry is stored or deflated
// (inflated by zlib) and checked against its CRC-32. Extracting writes every entry under a folder,
// refusing the whole archive before anything is written when a name would land outside it. Not
// read: Zip64 (over 4 GB or 65535 entries), encrypted entries, and compression methods other than
// stored and deflate.
module;
#include "Core/Prelude.h"

export module foundation.zip;

import foundation.core;

using namespace foundation::core;

export namespace foundation::zip
{
    /// One file or folder in an archive, as its central directory names it.
    struct ZipEntry
    {
        String name; ///< its path in the archive, with forward slashes ("Scenes/Level1.scene")
        u64 size = 0;           ///< its bytes once extracted
        u64 compressedSize = 0; ///< its bytes in the archive
        u32 crc = 0;            ///< the CRC-32 of its extracted bytes
        u16 method = 0;         ///< 0 stored, 8 deflate
        u64 localHeader = 0;    ///< where its local header starts
        bool directory = false; ///< a folder (its name ends with '/')
    };

    /// The CRC-32 (IEEE 802.3, as zip uses) of `bytes`, continuing from `crc`.
    [[nodiscard]] u32 Crc32(Span<const byte> bytes, u32 crc = 0);

    /// Whether an entry's name stays inside the folder it is extracted to: a relative path with no
    /// "..", no drive letter, and no empty name. Backslashes count as separators.
    [[nodiscard]] bool IsSafeEntryName(StringView name);

    class ZipArchive
    {
    public:
        /// Reads the central directory of the zip in `stream`, which must outlive the archive.
        /// False with `error`, said for a person, when it is not a zip this reads.
        bool Open(IStream& stream, String& error);

        [[nodiscard]] Span<const ZipEntry> Entries() const noexcept { return m_entries.AsSpan(); }
        /// The extracted bytes of all its files together.
        [[nodiscard]] u64 TotalSize() const noexcept;

        /// An entry's bytes, inflated and checked against its CRC. False with `error` when they
        /// cannot be read, do not inflate, or do not match.
        bool Read(const ZipEntry& entry, Array<byte>& out, String& error);

        /// Writes every entry under `directory` (made as needed), telling `progress` the bytes
        /// written of the total after each file. Every name is checked first: one that would land
        /// outside `directory` refuses the archive, and nothing is written. False with `error` at
        /// the first failure (the files before it stay).
        bool ExtractAll(StringView directory, String& error, const Function<void(u64 done, u64 total)>& progress = {});

    private:
        IStream* m_stream = nullptr;
        Array<ZipEntry> m_entries;
    };
}
