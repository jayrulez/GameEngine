// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// RHI logging. The backends log printf-style char* messages (LogError, LogWarning, LogErrorf,
// LogWarningf and the Info forms); these route them to the console, through an optional sink.

module;
#include "Core/Prelude.h"
#include <cstdio>
#include <cstdarg>

export module foundation.rhi:log;

import foundation.core;

using namespace foundation::core;

export namespace foundation::rhi
{
    /// Optional sink every RHI log line passes through before the console: the validation
    /// layer's diagnostics are observable in tests this way (its Submit reports return void).
    /// Return true to swallow the line. This is process-wide state, so the slot lives in an
    /// implementation unit (LogImpl.cpp) rather than inline here, where each shared library
    /// would get its own copy.
    using LogSink = bool (*)(void* context, bool error, const char* utf8);
    void SetLogSink(LogSink sink, void* context) noexcept;
    [[nodiscard]] bool DispatchToLogSink(bool error, const char* utf8) noexcept;

    inline void LogWrite(bool error, const char* utf8)
    {
        if (DispatchToLogSink(error, utf8))
        {
            return;
        }
        const StringView view(reinterpret_cast<const utf8char*>(utf8));
        if (error)
        {
            ConsoleWriteError(view);
        }
        else
        {
            ConsoleWrite(view);
        }
        ConsoleWrite(u8"\n");
    }

    inline void LogError(const char* message) { LogWrite(true, message); }
    inline void LogWarning(const char* message) { LogWrite(false, message); }
    inline void LogInfo(const char* message) { LogWrite(false, message); }

    inline void LogErrorf(const char* fmt, ...)
    {
        char buf[1024];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        LogWrite(true, buf);
    }

    inline void LogWarningf(const char* fmt, ...)
    {
        char buf[1024];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        LogWrite(false, buf);
    }

    inline void LogInfof(const char* fmt, ...)
    {
        char buf[1024];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        LogWrite(false, buf);
    }
}
