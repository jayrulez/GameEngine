// RHI logging shim. The ported backends use printf-style char* logging
// (logError / logWarning / logErrorf / logWarningf); this routes those to
// Draconic's console. A thin compatibility layer so the large backend bodies
// port unchanged.

module;
#include "Draconic.Foundation/Prelude.h"
#include <cstdio>
#include <cstdarg>

export module draconic.rhi:log;

import draconic.foundation;

using namespace draconic::foundation;

export namespace draconic::rhi
{
    inline void LogWrite(bool error, const char* utf8)
    {
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
