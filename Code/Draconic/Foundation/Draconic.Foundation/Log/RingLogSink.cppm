// Draconic Foundation - :ring_log_sink partition
//
// RingLogSink: keeps the most recent records in a ring buffer (tools / in-app
// consoles). Fixed-size records; long category/message text is truncated.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.foundation:ring_log_sink;

import :base;
import :allocator;
import :string;
import :ring_buffer;
import :logger;

export namespace draconic::foundation
{
    // Keeps the most recent messages in a ring buffer (for tools / in-app
    // consoles). Fixed-size records; long category/message text is truncated.
    struct LogRecord
    {
        LogLevel level;
        utf8char category[64];
        utf8char message[192];
    };

    class RingLogSink final : public ILogSink
    {
    public:
        explicit RingLogSink(usize capacity, IAllocator& allocator = DefaultAllocator())
            : m_records(capacity, allocator)
        {
        }

        void Write(LogLevel level, StringView category, StringView message) noexcept override
        {
            LogRecord record{};
            record.level = level;
            CopyTruncated(record.category, sizeof(record.category) / sizeof(utf8char), category);
            CopyTruncated(record.message, sizeof(record.message) / sizeof(utf8char), message);

            if (m_records.IsFull())
            {
                LogRecord discarded;
                m_records.PopFront(discarded);
            }
            m_records.PushBack(record);
        }

        [[nodiscard]] usize Count() const noexcept { return m_records.Size(); }
        [[nodiscard]] const LogRecord& Record(usize index) const noexcept
        {
            return m_records[index];
        }

    private:
        static void CopyTruncated(utf8char* dst, usize dstCount, StringView src) noexcept
        {
            const usize n = (src.Size() < dstCount - 1) ? src.Size() : dstCount - 1;
            MemCopy(dst, src.Data(), n * sizeof(utf8char));
            dst[n] = utf8char('\0');
        }

        RingBuffer<LogRecord> m_records;
    };
}
