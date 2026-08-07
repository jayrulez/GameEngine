// Draconic Foundation - Debug / assertions
//
// Assertions are macro-based, so they live in a classic header (macros cannot
// be exported by modules). The reporting functions have external linkage in the
// global module, which lets *any* translation unit - including other module
// units' global-module-fragments (e.g. :base) - include this and call them with
// no module-import cycle.
//
// Usage: `#include "Draconic.Foundation/Debug/Assert.h"` in a global module fragment.
//
//   DRACONIC_ASSERT(cond)        debug-only; report + break on failure
//   DRACONIC_ASSERT_MSG(c, msg)  ditto, with a message
//   DRACONIC_VERIFY(cond)        condition always evaluated; checked unless shipping
//   DRACONIC_CHECK(cond)         always-on assert (all build configs)
//   DRACONIC_ENSURE(cond)        non-fatal; reports once on failure, returns the bool
//   DRACONIC_UNREACHABLE()       fatal; marks unreachable code

#ifndef DRACONIC_FOUNDATION_DEBUG_ASSERT_H
#define DRACONIC_FOUNDATION_DEBUG_ASSERT_H

#include "Draconic.Foundation/Prelude.h"

namespace draconic::foundation
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

#define DRACONIC_DEBUGBREAK() ::draconic::foundation::DebugBreak()

// Foundation check expression: evaluates `cond`; on failure reports and, if the
// handler requests it, breaks. Yields void.
#define DRACONIC_ASSERT_IMPL(cond, msg)                                                            \
    (DRACONIC_LIKELY(!!(cond))                                                                     \
         ? (void)0                                                                                 \
         : (::draconic::foundation::ReportAssertFailure(#cond, (msg), __FILE__, __LINE__, __func__)      \
                ? DRACONIC_DEBUGBREAK()                                                            \
                : (void)0))

#if DRACONIC_SHIPPING
// Disabled asserts still REFERENCE the expression, unevaluated (sizeof of a ternary):
// zero codegen, but parameters/locals used only in asserts stay "used" - otherwise every
// assert-only parameter breaks the shipping build under -Werror=unused-parameter.
#define DRACONIC_ASSERT(cond) ((void)sizeof((cond) ? 1 : 0))
#define DRACONIC_ASSERT_MSG(cond, msg) ((void)sizeof((cond) ? 1 : 0))
#define DRACONIC_VERIFY(cond) ((void)(cond))
#else
#define DRACONIC_ASSERT(cond) DRACONIC_ASSERT_IMPL(cond, nullptr)
#define DRACONIC_ASSERT_MSG(cond, msg) DRACONIC_ASSERT_IMPL(cond, msg)
#define DRACONIC_VERIFY(cond) DRACONIC_ASSERT_IMPL(cond, nullptr)
#endif

// Always-on, every build configuration.
#define DRACONIC_CHECK(cond) DRACONIC_ASSERT_IMPL(cond, nullptr)
#define DRACONIC_CHECK_MSG(cond, msg) DRACONIC_ASSERT_IMPL(cond, msg)

// Non-fatal: reports on failure but does not break; evaluates to the condition,
// so it composes:  if (!DRACONIC_ENSURE(ptr != nullptr)) { return; }
#define DRACONIC_ENSURE(cond)                                                                      \
    (DRACONIC_LIKELY(!!(cond))                                                                     \
         ? true                                                                                    \
         : (::draconic::foundation::ReportAssertFailure(#cond, nullptr, __FILE__, __LINE__, __func__),   \
            false))

#define DRACONIC_UNREACHABLE()                                                                     \
    (::draconic::foundation::ReportFatal("reached unreachable code", __FILE__, __LINE__, __func__))

#endif // DRACONIC_FOUNDATION_DEBUG_ASSERT_H
