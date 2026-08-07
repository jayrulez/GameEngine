// Draconic Foundation - :file_sink partition
//
// FileSink: appends formatted lines to a file.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.foundation:file_sink;

import :base;
import :string;
import :system;
import :logger;

export namespace draconic::foundation
{
    // Appends to a file.
    class FileSink final : public ILogSink
    {
    public:
        explicit FileSink(StringView path) noexcept { m_file = FileOpen(path, FileMode::Append); }
        ~FileSink() override
        {
            if (FileIsValid(m_file))
            {
                FileClose(m_file);
            }
        }

        FileSink(const FileSink&) = delete;
        FileSink& operator=(const FileSink&) = delete;

        [[nodiscard]] bool IsOpen() const noexcept { return FileIsValid(m_file); }

        void Write(LogLevel level, StringView category, StringView message) noexcept override
        {
            if (!FileIsValid(m_file))
            {
                return;
            }

            String line;
            detail::FormatLine(line, level, category, message);
            // Strings are already UTF-8; write directly.
            (void)FileWrite(m_file, line.Data(), line.Size());
        }

    private:
        FileHandle m_file = kInvalidFile;
    };
}
