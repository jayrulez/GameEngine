// Draconic Foundation - :console_sink partition
//
// ConsoleSink: writes formatted lines to stdout (stderr for Error/Fatal).

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.foundation:console_sink;

import :base;
import :string;
import :system;
import :logger;

export namespace draconic::foundation
{
    // Writes to stdout (stderr for Error/Fatal).
    class ConsoleSink final : public ILogSink
    {
    public:
        void Write(LogLevel level, StringView category, StringView message) noexcept override
        {
            String line;
            detail::FormatLine(line, level, category, message);

            if (static_cast<u8>(level) >= static_cast<u8>(LogLevel::Error))
            {
                ConsoleWriteError(line);
            }
            else
            {
                ConsoleWrite(line);
            }
        }
    };
}
