// Draconic Foundation - Debug runtime (classic TU; see Assert.h for the rationale).

#include "Draconic.Foundation/Debug/Assert.h"
#include "Draconic.Foundation/System/SystemBackend.h"

#include <cstdio>
#include <cstdlib>

namespace draconic::foundation
{
    namespace
    {
        AssertHandler g_assertHandler = nullptr;

        // Best-effort native stack dump so a crash in the wild is actionable from the
        // console output alone. Platform work lives in the System backend.
        void PrintStackTrace() noexcept
        {
            std::fprintf(stderr, "  backtrace (addr2line -e <binary> -f -C to resolve):\n");
            std::fflush(stderr);
            (void)sys::WriteBacktrace(2 /* stderr */);
        }
    }

    AssertHandler GetAssertHandler() noexcept { return g_assertHandler; }

    void SetAssertHandler(AssertHandler handler) noexcept { g_assertHandler = handler; }

    void DebugBreak() noexcept
    {
#if DRACONIC_COMPILER_MSVC && !DRACONIC_COMPILER_CLANG
        __debugbreak();
#else
        __builtin_trap();
#endif
    }

    bool ReportAssertFailure(const char* expression, const char* message, const char* file,
                             int line, const char* function) noexcept
    {
        if (g_assertHandler != nullptr)
        {
            return g_assertHandler(expression, message, file, line, function);
        }

        std::fprintf(stderr,
                     "\nDraconic assertion failed\n"
                     "  expression: %s\n"
                     "  message   : %s\n"
                     "  location  : %s:%d\n"
                     "  function  : %s\n",
                     expression, (message != nullptr) ? message : "(none)", file, line, function);
        std::fflush(stderr);
        PrintStackTrace();
        return true; // break into the debugger / trap
    }

    void ReportFatal(const char* message, const char* file, int line, const char* function) noexcept
    {
        std::fprintf(stderr,
                     "\nDraconic fatal error\n"
                     "  message : %s\n"
                     "  location: %s:%d\n"
                     "  function: %s\n",
                     message, file, line, function);
        std::fflush(stderr);
        PrintStackTrace();
        std::abort();
    }
}
