// Draconic Foundation - :logger partition
//
// Logging frontend: log levels, the ILogSink interface, the Logger (sink list
// + level filter) and the Logf frontend behind the DRACONIC_LOG_* macros.
// Concrete sinks (Console/File/Ring) live in their own partitions.
//
// Thread-safe: an atomic level filters cheaply on the hot path; a mutex guards
// sink registration and dispatch.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.foundation:logger;

import :base;
import :array;
import :string;
import :format;
import :atomic;
import :mutex;
import :scoped_lock;

export namespace draconic::foundation
{
    enum class LogLevel : u8
    {
        Trace,
        Debug,
        Info,
        Warning,
        Error,
        Fatal,
        Off, // sentinel: filters out everything
    };

    [[nodiscard]] inline const utf8char* LogLevelName(LogLevel level) noexcept
    {
        switch (level)
        {
        case LogLevel::Trace:
            return u8"Trace";
        case LogLevel::Debug:
            return u8"Debug";
        case LogLevel::Info:
            return u8"Info";
        case LogLevel::Warning:
            return u8"Warning";
        case LogLevel::Error:
            return u8"Error";
        case LogLevel::Fatal:
            return u8"Fatal";
        case LogLevel::Off:
            return u8"Off";
        }
        return u8"?";
    }

    // -----------------------------------------------------------------------
    // Sinks
    // -----------------------------------------------------------------------
    class ILogSink
    {
    public:
        virtual ~ILogSink() = default;
        virtual void Write(LogLevel level, StringView category, StringView message) noexcept = 0;
    };

    namespace detail
    {
        inline void FormatLine(String& line, LogLevel level, StringView category,
                               StringView message)
        {
            // "[Level] category: message\n"
            line.Clear();
            line.PushBack(utf8char('['));
            line.Append(StringView(LogLevelName(level)));
            line.PushBack(utf8char(']'));
            line.PushBack(utf8char(' '));
            line.Append(category);
            line.PushBack(utf8char(':'));
            line.PushBack(utf8char(' '));
            line.Append(message);
            line.PushBack(utf8char('\n'));
        }
    }

    // -----------------------------------------------------------------------
    // Logger - owns the sink list and the active level filter.
    // -----------------------------------------------------------------------
    // Thread-safe: an atomic level filters cheaply on the hot path; a mutex
    // guards sink registration and dispatch.
    class Logger
    {
    public:
        // Sinks are non-owning; the caller manages their lifetime.
        void AddSink(ILogSink* sink)
        {
            if (sink == nullptr)
            {
                return;
            }
            ScopedLock lock(m_mutex);
            m_sinks.PushBack(sink);
        }

        void RemoveSink(ILogSink* sink) noexcept
        {
            ScopedLock lock(m_mutex);
            for (usize i = 0; i < m_sinks.Size(); ++i)
            {
                if (m_sinks[i] == sink)
                {
                    m_sinks.RemoveAtSwap(i);
                    return;
                }
            }
        }

        void SetMinLevel(LogLevel level) noexcept { m_minLevel.store(level); }
        [[nodiscard]] LogLevel MinLevel() const noexcept { return m_minLevel.load(); }

        [[nodiscard]] bool IsEnabled(LogLevel level) const noexcept
        {
            return static_cast<u8>(level) >= static_cast<u8>(m_minLevel.load());
        }

        void Dispatch(LogLevel level, StringView category, StringView message) noexcept
        {
            ScopedLock lock(m_mutex);
            for (ILogSink* sink : m_sinks)
            {
                sink->Write(level, category, message);
            }
        }

    private:
        Array<ILogSink*> m_sinks;
        Atomic<LogLevel> m_minLevel{LogLevel::Info};
        Mutex m_mutex;
    };

    [[nodiscard]] Logger& GlobalLogger() noexcept
    {
        static Logger instance;
        return instance;
    }

    // -----------------------------------------------------------------------
    // Frontend - formats and dispatches (used by the DRACONIC_LOG_* macros).
    // The format buffer is UTF-8, matching the Logger/sink UTF-8 API.
    // -----------------------------------------------------------------------
    template <typename... Args>
    void Logf(LogLevel level, StringView category, FormatString<Args...> fmt, const Args&... args)
    {
        Logger& logger = GlobalLogger();
        if (!logger.IsEnabled(level))
        {
            return;
        }

        FormatBuffer buffer;
        FormatToV(buffer, fmt.data, args...);
        const String message = String(buffer.View());
        logger.Dispatch(level, category, message);
    }
}
