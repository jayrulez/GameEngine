// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Core - :binary_io partition
//
// BinaryWriter / BinaryReader: thin typed wrappers over an IStream for raw
// binary I/O. They track a sticky "ok" flag (set false on a short transfer) so
// callers can do a batch of reads/writes and check success once at the end.
// BinarySerializer is built on these.

module;
#include "Core/Prelude.h"
#include <cstring>
#include <type_traits>

export module foundation.core:binary_io;

import :base;
import :string;
import :io;

export namespace foundation::core
{
    // =======================================================================
    // BinaryWriter - typed binary output over an IStream.
    // =======================================================================
    class BinaryWriter
    {
    public:
        explicit BinaryWriter(IStream& stream) noexcept : m_stream(&stream) {}

        [[nodiscard]] bool IsOk() const noexcept { return m_ok; }

        // Raw bytes. Sticks to !IsOk() on a short write.
        bool WriteBytes(const void* data, usize size)
        {
            if (size == 0)
            {
                return m_ok;
            }
            if (m_stream->Write(data, size) != size)
            {
                m_ok = false;
            }
            return m_ok;
        }

        // One trivially-copyable value, as-stored.
        template <typename T>
        bool Write(const T& value)
        {
            static_assert(std::is_trivially_copyable_v<T>,
                          "BinaryWriter::Write requires a trivially-copyable type.");
            return WriteBytes(&value, sizeof(T));
        }

        // Length-prefixed (u32 count) UTF-8 string.
        bool WriteString(StringView value)
        {
            const u32 length = static_cast<u32>(value.Size());
            Write(length);
            if (length > 0)
            {
                WriteBytes(value.Data(), static_cast<usize>(length) * sizeof(utf8char));
            }
            return m_ok;
        }

    private:
        IStream* m_stream;
        bool m_ok = true;
    };

    // =======================================================================
    // BinaryReader - typed binary input over an IStream.
    // =======================================================================
    class BinaryReader
    {
    public:
        explicit BinaryReader(IStream& stream) noexcept : m_stream(&stream) {}

        [[nodiscard]] bool IsOk() const noexcept { return m_ok; }

        // Raw bytes. Sticks to !IsOk() on a short read; once failed it reads nothing more and
        // hands back zeros, so a count read after the failure is 0 and a loop over it ends.
        bool ReadBytes(void* data, usize size)
        {
            if (size == 0)
            {
                return m_ok;
            }
            if (!m_ok)
            {
                std::memset(data, 0, size);
                return false;
            }
            if (m_stream->Read(data, size) != size)
            {
                m_ok = false;
            }
            return m_ok;
        }

        // One trivially-copyable value, as-stored.
        template <typename T>
        bool Read(T& outValue)
        {
            static_assert(std::is_trivially_copyable_v<T>,
                          "BinaryReader::Read requires a trivially-copyable type.");
            return ReadBytes(&outValue, sizeof(T));
        }

        // Length-prefixed (u32 count) UTF-8 string.
        bool ReadString(String& outValue)
        {
            u32 length = 0;
            Read(length);
            outValue.Clear();
            if (!m_ok)
            {
                return false;
            }
            if (!Holds(length))
            {
                m_ok = false;
                return false;
            }

            outValue.Reserve(length);
            for (u32 i = 0; i < length; ++i)
            {
                utf8char ch{};
                if (!ReadBytes(&ch, sizeof(utf8char)))
                {
                    break;
                }
                outValue.PushBack(ch);
            }
            return m_ok;
        }

        // Whether `count` items of at least a byte each can still be in the stream. A count
        // beyond the bytes left is a misread (a record written in another layout), not data:
        // trusting it reserves gigabytes and reads past the end millions of times. A count up
        // to kTrustedCount passes without asking the stream (a file stream's size and position
        // are two system calls), since reading it out costs little before the end stops it.
        [[nodiscard]] bool Holds(u64 count) const
        {
            if (count <= kTrustedCount)
            {
                return true;
            }
            const i64 size = m_stream->Size();
            const i64 at = m_stream->Tell();
            return size < 0 || at < 0 || count <= static_cast<u64>(size > at ? size - at : 0);
        }

    private:
        static constexpr u64 kTrustedCount = 4096;

        IStream* m_stream;
        bool m_ok = true;
    };
}
