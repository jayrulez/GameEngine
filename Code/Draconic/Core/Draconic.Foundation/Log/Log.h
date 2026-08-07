// Draconic Foundation - logging macros (classic header).
//
// Convenience front-end over draconic::foundation::Logf. Include this and `import
// Draconic.Foundation;`. The macros are stripped at LogLevel::Fatal+ granularity in
// shipping builds (asserts/logging policy, §4.8).
//
//   DRACONIC_LOG(level, category, "fmt {} {}", a, b);
//   DRACONIC_LOG_INFO("Renderer", "loaded {} meshes", count);

#ifndef DRACONIC_FOUNDATION_LOG_H
#define DRACONIC_FOUNDATION_LOG_H

#include "Draconic.Foundation/Prelude.h"

#define DRACONIC_LOG(level, category, ...) ::draconic::foundation::Logf((level), (category), __VA_ARGS__)

#if DRACONIC_SHIPPING
// Strip verbose levels in shipping; keep Warning and above. The dead ternary arm keeps
// the arguments REFERENCED (type-checked, never evaluated, constant-folded to nothing) -
// otherwise every log-only parameter breaks the shipping build under -Werror=unused.
#define DRACONIC_LOG_TRACE(category, ...)                                                          \
    (true ? (void)0 : DRACONIC_LOG(::draconic::foundation::LogLevel::Trace, (category), __VA_ARGS__))
#define DRACONIC_LOG_DEBUG(category, ...)                                                          \
    (true ? (void)0 : DRACONIC_LOG(::draconic::foundation::LogLevel::Debug, (category), __VA_ARGS__))
#define DRACONIC_LOG_INFO(category, ...)                                                           \
    (true ? (void)0 : DRACONIC_LOG(::draconic::foundation::LogLevel::Info, (category), __VA_ARGS__))
#else
#define DRACONIC_LOG_TRACE(category, ...)                                                          \
    DRACONIC_LOG(::draconic::foundation::LogLevel::Trace, (category), __VA_ARGS__)
#define DRACONIC_LOG_DEBUG(category, ...)                                                          \
    DRACONIC_LOG(::draconic::foundation::LogLevel::Debug, (category), __VA_ARGS__)
#define DRACONIC_LOG_INFO(category, ...)                                                           \
    DRACONIC_LOG(::draconic::foundation::LogLevel::Info, (category), __VA_ARGS__)
#endif

#define DRACONIC_LOG_WARNING(category, ...)                                                        \
    DRACONIC_LOG(::draconic::foundation::LogLevel::Warning, (category), __VA_ARGS__)
#define DRACONIC_LOG_ERROR(category, ...)                                                          \
    DRACONIC_LOG(::draconic::foundation::LogLevel::Error, (category), __VA_ARGS__)
#define DRACONIC_LOG_FATAL(category, ...)                                                          \
    DRACONIC_LOG(::draconic::foundation::LogLevel::Fatal, (category), __VA_ARGS__)

#endif // DRACONIC_FOUNDATION_LOG_H
