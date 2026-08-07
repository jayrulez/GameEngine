// Draconic Foundation - :io partition (streams)
//
// IStream is the read/write/seek abstraction; FileStream wraps the System file
// primitives, MemoryStream is an in-memory growable buffer. The serialization
// contract (ISerializer) builds on these next.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Debug/Assert.h"
#include <type_traits>

export module draconic.foundation:io;

import :base;
import :allocator;
import :array;
import :span;
import :string;
import :system;

export namespace draconic::foundation
{
    class IStream
    {
    public:
        virtual ~IStream() = default;

        // Bytes actually transferred (may be < requested at EOF / on error).
        [[nodiscard]] virtual u64 Read(void* destination, u64 bytes) = 0;
        [[nodiscard]] virtual u64 Write(const void* source, u64 bytes) = 0;

        // New absolute position, or -1 on error.
        [[nodiscard]] virtual i64 Seek(i64 offset, SeekOrigin origin) = 0;
        [[nodiscard]] virtual i64 Tell() const = 0;
        [[nodiscard]] virtual i64 Size() const = 0;
        [[nodiscard]] virtual bool IsValid() const = 0;

        // Typed convenience for trivially-copyable values.
        template <typename T>
        [[nodiscard]] bool WriteValue(const T& value)
        {
            static_assert(std::is_trivially_copyable_v<T>,
                          "WriteValue requires a trivially-copyable type.");
            return Write(&value, sizeof(T)) == sizeof(T);
        }

        template <typename T>
        [[nodiscard]] bool ReadValue(T& outValue)
        {
            static_assert(std::is_trivially_copyable_v<T>,
                          "ReadValue requires a trivially-copyable type.");
            return Read(&outValue, sizeof(T)) == sizeof(T);
        }
    };

    // =======================================================================
    // FileStream - IStream over a System file handle.
    // =======================================================================
    class FileStream final : public IStream
    {
    public:
        FileStream(StringView path, FileMode mode) noexcept { m_file = FileOpen(path, mode); }

        ~FileStream() override
        {
            if (FileIsValid(m_file))
            {
                FileClose(m_file);
            }
        }

        FileStream(const FileStream&) = delete;
        FileStream& operator=(const FileStream&) = delete;

        [[nodiscard]] bool IsValid() const override { return FileIsValid(m_file); }

        [[nodiscard]] u64 Read(void* destination, u64 bytes) override
        {
            const i64 n = FileRead(m_file, destination, bytes);
            return (n < 0) ? 0 : static_cast<u64>(n);
        }

        [[nodiscard]] u64 Write(const void* source, u64 bytes) override
        {
            const i64 n = FileWrite(m_file, source, bytes);
            return (n < 0) ? 0 : static_cast<u64>(n);
        }

        [[nodiscard]] i64 Seek(i64 offset, SeekOrigin origin) override
        {
            return FileSeek(m_file, offset, origin);
        }

        [[nodiscard]] i64 Tell() const override { return FileSeek(m_file, 0, SeekOrigin::Current); }
        [[nodiscard]] i64 Size() const override { return FileSize(m_file); }

    private:
        FileHandle m_file = kInvalidFile;
    };

    // =======================================================================
    // MemoryStream - IStream over a growable in-memory byte buffer.
    // =======================================================================
    class MemoryStream final : public IStream
    {
    public:
        MemoryStream() = default;
        explicit MemoryStream(IAllocator& allocator) : m_data(allocator) {}

        [[nodiscard]] bool IsValid() const override { return true; }

        [[nodiscard]] u64 Read(void* destination, u64 bytes) override
        {
            const u64 available = static_cast<u64>(m_data.Size()) - m_position;
            const u64 toRead = (bytes < available) ? bytes : available;
            if (toRead > 0)
            {
                MemCopy(destination, &m_data[static_cast<usize>(m_position)],
                        static_cast<usize>(toRead));
                m_position += toRead;
            }
            return toRead;
        }

        [[nodiscard]] u64 Write(const void* source, u64 bytes) override
        {
            if (bytes == 0)
            {
                return 0;
            }

            const u64 end = m_position + bytes;
            if (end > static_cast<u64>(m_data.Size()))
            {
                // Grow capacity geometrically so a run of small writes (e.g. byte-at-a-time
                // serialization of a large blob) stays amortized O(1) instead of O(n) per write
                // (Array::Reserve allocates exactly, so Resize-to-exact each call would be O(n^2)).
                if (end > static_cast<u64>(m_data.Capacity()))
                {
                    usize cap = (m_data.Capacity() == 0) ? 64u : m_data.Capacity();
                    while (static_cast<u64>(cap) < end)
                    {
                        cap *= 2u;
                    }
                    m_data.Reserve(cap);
                }
                m_data.Resize(static_cast<usize>(end));
            }
            MemCopy(&m_data[static_cast<usize>(m_position)], source, static_cast<usize>(bytes));
            m_position += bytes;
            return bytes;
        }

        [[nodiscard]] i64 Seek(i64 offset, SeekOrigin origin) override
        {
            i64 base = 0;
            switch (origin)
            {
            case SeekOrigin::Begin:
                base = 0;
                break;
            case SeekOrigin::Current:
                base = static_cast<i64>(m_position);
                break;
            case SeekOrigin::End:
                base = static_cast<i64>(m_data.Size());
                break;
            }
            const i64 target = base + offset;
            if (target < 0 || target > static_cast<i64>(m_data.Size()))
            {
                return -1;
            }
            m_position = static_cast<u64>(target);
            return target;
        }

        [[nodiscard]] i64 Tell() const override { return static_cast<i64>(m_position); }
        [[nodiscard]] i64 Size() const override { return static_cast<i64>(m_data.Size()); }

        [[nodiscard]] Span<const byte> Bytes() const noexcept
        {
            return Span<const byte>{m_data.Data(), m_data.Size()};
        }

        void Clear() noexcept
        {
            m_data.Clear();
            m_position = 0;
        }

    private:
        Array<byte> m_data;
        u64 m_position = 0;
    };

    // =======================================================================
    // BufferedStream - buffers reads/writes over an underlying IStream to cut
    // the number of small transfers. Buffers in one direction at a time;
    // switching direction (or seeking) syncs the buffer first. The underlying
    // stream must outlive the BufferedStream.
    // =======================================================================
    class BufferedStream final : public IStream
    {
    public:
        explicit BufferedStream(IStream& stream, usize bufferSize = 4096) : m_stream(&stream)
        {
            m_buffer.Resize(bufferSize == 0 ? 1 : bufferSize);
        }

        BufferedStream(const BufferedStream&) = delete;
        BufferedStream& operator=(const BufferedStream&) = delete;

        ~BufferedStream() override { FlushWrites(); }

        [[nodiscard]] bool IsValid() const override { return m_stream->IsValid(); }

        // Flushes any pending writes to the underlying stream.
        void Flush() { FlushWrites(); }

        [[nodiscard]] u64 Write(const void* source, u64 bytes) override
        {
            if (m_mode == Mode::Read)
            {
                SyncForSeek();
            }
            m_mode = Mode::Write;

            const byte* src = static_cast<const byte*>(source);
            u64 remaining = bytes;
            while (remaining > 0)
            {
                const usize space = m_buffer.Size() - m_pos;
                const usize chunk = (remaining < space) ? static_cast<usize>(remaining) : space;
                MemCopy(&m_buffer[m_pos], src, chunk);
                m_pos += chunk;
                src += chunk;
                remaining -= chunk;
                if (m_pos == m_buffer.Size())
                {
                    FlushWrites();
                }
            }
            return bytes;
        }

        [[nodiscard]] u64 Read(void* destination, u64 bytes) override
        {
            if (m_mode == Mode::Write)
            {
                FlushWrites();
            }
            m_mode = Mode::Read;

            byte* dst = static_cast<byte*>(destination);
            u64 produced = 0;
            while (produced < bytes)
            {
                if (m_pos == m_len)
                {
                    m_len = static_cast<usize>(m_stream->Read(m_buffer.Data(), m_buffer.Size()));
                    m_pos = 0;
                    if (m_len == 0)
                    {
                        break;
                    } // EOF
                }
                const usize available = m_len - m_pos;
                const u64 want = bytes - produced;
                const usize chunk = (want < available) ? static_cast<usize>(want) : available;
                MemCopy(dst + produced, &m_buffer[m_pos], chunk);
                m_pos += chunk;
                produced += chunk;
            }
            return produced;
        }

        [[nodiscard]] i64 Seek(i64 offset, SeekOrigin origin) override
        {
            SyncForSeek();
            return m_stream->Seek(offset, origin);
        }

        [[nodiscard]] i64 Tell() const override
        {
            const i64 base = m_stream->Tell();
            if (m_mode == Mode::Write)
            {
                return base + static_cast<i64>(m_pos);
            }
            if (m_mode == Mode::Read)
            {
                return base - static_cast<i64>(m_len - m_pos);
            }
            return base;
        }

        // Note: ignores unflushed pending writes that may extend the file.
        [[nodiscard]] i64 Size() const override { return m_stream->Size(); }

    private:
        enum class Mode
        {
            None,
            Read,
            Write
        };

        void FlushWrites()
        {
            if (m_mode == Mode::Write && m_pos > 0)
            {
                (void)m_stream->Write(m_buffer.Data(), m_pos);
            }
            m_pos = 0;
            m_len = 0;
            m_mode = Mode::None;
        }

        void SyncForSeek()
        {
            if (m_mode == Mode::Write)
            {
                FlushWrites();
            }
            else if (m_mode == Mode::Read)
            {
                const i64 unread = static_cast<i64>(m_len - m_pos);
                if (unread > 0)
                {
                    (void)m_stream->Seek(-unread, SeekOrigin::Current);
                }
            }
            m_pos = 0;
            m_len = 0;
            m_mode = Mode::None;
        }

        IStream* m_stream;
        Array<byte> m_buffer;
        usize m_pos = 0; // write: bytes pending; read: cursor into buffer
        usize m_len = 0; // read: valid bytes prefetched
        Mode m_mode = Mode::None;
    };
}
