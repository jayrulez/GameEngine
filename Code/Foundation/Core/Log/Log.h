// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Core - logging macros (classic header).
//
// Convenience front-end over foundation::core::Logf. Include this and `import
// foundation.core;`. In shipping builds LOG_TRACE, LOG_DEBUG and LOG_INFO compile to
// nothing; Warning and above stay.
//
//   LOG(level, category, "fmt {} {}", a, b);
//   LOG_INFO("Renderer", "loaded {} meshes", count);

#ifndef FOUNDATION_CORE_LOG_H
#define FOUNDATION_CORE_LOG_H

#include "Core/Prelude.h"

#define LOG(level, category, ...) ::foundation::core::Logf((level), (category), __VA_ARGS__)

#if BUILD_SHIPPING
// Strip verbose levels in shipping; keep Warning and above. The dead ternary arm keeps
// the arguments referenced (type-checked, never evaluated, constant-folded to nothing).
// Otherwise every log-only parameter breaks the shipping build under -Werror=unused.
#define LOG_TRACE(category, ...)                                                          \
    (true ? (void)0 : LOG(::foundation::core::LogLevel::Trace, (category), __VA_ARGS__))
#define LOG_DEBUG(category, ...)                                                          \
    (true ? (void)0 : LOG(::foundation::core::LogLevel::Debug, (category), __VA_ARGS__))
#define LOG_INFO(category, ...)                                                           \
    (true ? (void)0 : LOG(::foundation::core::LogLevel::Info, (category), __VA_ARGS__))
#else
#define LOG_TRACE(category, ...)                                                          \
    LOG(::foundation::core::LogLevel::Trace, (category), __VA_ARGS__)
#define LOG_DEBUG(category, ...)                                                          \
    LOG(::foundation::core::LogLevel::Debug, (category), __VA_ARGS__)
#define LOG_INFO(category, ...)                                                           \
    LOG(::foundation::core::LogLevel::Info, (category), __VA_ARGS__)
#endif

#define LOG_WARNING(category, ...)                                                        \
    LOG(::foundation::core::LogLevel::Warning, (category), __VA_ARGS__)
#define LOG_ERROR(category, ...)                                                          \
    LOG(::foundation::core::LogLevel::Error, (category), __VA_ARGS__)
#define LOG_FATAL(category, ...)                                                          \
    LOG(::foundation::core::LogLevel::Fatal, (category), __VA_ARGS__)

#endif // FOUNDATION_CORE_LOG_H
