// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Core - Debug / assertions
//
// Assertions are macro-based, so they live in a classic header (macros cannot
// be exported by modules). The reporting functions have external linkage in the
// global module, which lets any translation unit (including the global module
// fragments of other module units, e.g. :base) include this and call them with
// no module-import cycle.
//
// Usage: `#include "Core/Debug/Assert.h"` in a global module fragment.
//
//   DIAGNOSTIC_ASSERT(cond)        debug-only; report + break on failure
//   DIAGNOSTIC_ASSERT_MSG(c, msg)  ditto, with a message
//   DIAGNOSTIC_VERIFY(cond)        condition always evaluated; checked unless shipping
//   DIAGNOSTIC_CHECK(cond)         always-on assert (all build configs)
//   DIAGNOSTIC_ENSURE(cond)        non-fatal; reports once on failure, returns the bool
//   DIAGNOSTIC_UNREACHABLE()       fatal; marks unreachable code

#ifndef FOUNDATION_CORE_DEBUG_ASSERT_H
#define FOUNDATION_CORE_DEBUG_ASSERT_H

#include "Core/Prelude.h"

namespace foundation::core
{
    // Reports a failed assertion. Returns true if the caller should break into
    // the debugger / trap. Plain const char* keeps this dependency-free and
    // safe to include in any global module fragment.
    bool ReportAssertFailure(const char* expression, const char* message, const char* file,
                             int line, const char* function) noexcept;

    // Reports a fatal error and terminates. Never returns.
    [[noreturn]] void ReportFatal(const char* message, const char* file, int line,
                                  const char* function) noexcept;

    // Custom assertion handler. Return true to break/trap, false to continue.
    using AssertHandler = bool (*)(const char* expression, const char* message, const char* file,
                                   int line, const char* function) noexcept;

    AssertHandler GetAssertHandler() noexcept;
    void SetAssertHandler(AssertHandler handler) noexcept;

    // Traps into the debugger / aborts. A real function (not a builtin macro)
    // so it is callable from module code without tripping Clang's
    // cross-module-builtin ambiguity.
    void DebugBreak() noexcept;
}

#define DIAGNOSTIC_DEBUGBREAK() ::foundation::core::DebugBreak()

// Core check expression: evaluates `cond`; on failure reports and, if the
// handler requests it, breaks. Yields void.
#define DIAGNOSTIC_ASSERT_IMPL(cond, msg)                                                            \
    (COMPILER_ATTR_LIKELY(!!(cond))                                                                     \
         ? (void)0                                                                                 \
         : (::foundation::core::ReportAssertFailure(#cond, (msg), __FILE__, __LINE__, __func__)      \
                ? DIAGNOSTIC_DEBUGBREAK()                                                            \
                : (void)0))

#if BUILD_SHIPPING
// Disabled asserts still reference the expression, unevaluated (sizeof of a ternary):
// zero codegen, but parameters and locals used only in asserts stay "used". Otherwise every
// assert-only parameter breaks the shipping build under -Werror=unused-parameter.
#define DIAGNOSTIC_ASSERT(cond) ((void)sizeof((cond) ? 1 : 0))
#define DIAGNOSTIC_ASSERT_MSG(cond, msg) ((void)sizeof((cond) ? 1 : 0))
#define DIAGNOSTIC_VERIFY(cond) ((void)(cond))
#else
#define DIAGNOSTIC_ASSERT(cond) DIAGNOSTIC_ASSERT_IMPL(cond, nullptr)
#define DIAGNOSTIC_ASSERT_MSG(cond, msg) DIAGNOSTIC_ASSERT_IMPL(cond, msg)
#define DIAGNOSTIC_VERIFY(cond) DIAGNOSTIC_ASSERT_IMPL(cond, nullptr)
#endif

// Always-on, every build configuration.
#define DIAGNOSTIC_CHECK(cond) DIAGNOSTIC_ASSERT_IMPL(cond, nullptr)
#define DIAGNOSTIC_CHECK_MSG(cond, msg) DIAGNOSTIC_ASSERT_IMPL(cond, msg)

// Non-fatal: reports on failure but does not break; evaluates to the condition,
// so it composes:  if (!DIAGNOSTIC_ENSURE(ptr != nullptr)) { return; }
#define DIAGNOSTIC_ENSURE(cond)                                                                      \
    (COMPILER_ATTR_LIKELY(!!(cond))                                                                     \
         ? true                                                                                    \
         : (::foundation::core::ReportAssertFailure(#cond, nullptr, __FILE__, __LINE__, __func__),   \
            false))

#define DIAGNOSTIC_UNREACHABLE()                                                                     \
    (::foundation::core::ReportFatal("reached unreachable code", __FILE__, __LINE__, __func__))

#endif // FOUNDATION_CORE_DEBUG_ASSERT_H
