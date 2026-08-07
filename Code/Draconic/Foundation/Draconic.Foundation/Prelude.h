// Draconic Foundation - Prelude
//
// The one classic header allowed to cross the module boundary. Holds everything
// macro-based (platform/compiler detection, attributes, build config), since
// preprocessor macros do not propagate through C++ modules. Each .cppm includes
// this in its global module fragment:
//
//     module;
//     #include "Draconic.Foundation/Prelude.h"
//     export module draconic.foundation:base;
//
// Keep this tiny, dependency-free, and include-once.

#ifndef DRACONIC_FOUNDATION_PRELUDE_H
#define DRACONIC_FOUNDATION_PRELUDE_H

// Placement operator new must be reachable in every TU that instantiates a
// container (GCC binds it at the instantiation site, not where the placement
// new textually appears). Prelude.h is included in every module's global module
// fragment, so this makes it uniformly available. <new> is a tiny header.
#include <new>

// ---------------------------------------------------------------------------
// Compiler detection
// ---------------------------------------------------------------------------
#if defined(__clang__)
#define DRACONIC_COMPILER_CLANG 1
#elif defined(__GNUC__)
#define DRACONIC_COMPILER_GCC 1
#elif defined(_MSC_VER)
#define DRACONIC_COMPILER_MSVC 1
#else
#error "Draconic: unsupported compiler."
#endif

#if !defined(DRACONIC_COMPILER_CLANG)
#define DRACONIC_COMPILER_CLANG 0
#endif
#if !defined(DRACONIC_COMPILER_GCC)
#define DRACONIC_COMPILER_GCC 0
#endif
#if !defined(DRACONIC_COMPILER_MSVC)
#define DRACONIC_COMPILER_MSVC 0
#endif

// ---------------------------------------------------------------------------
// Platform detection
// ---------------------------------------------------------------------------
#if defined(__EMSCRIPTEN__)
#define DRACONIC_PLATFORM_WEB 1
#elif defined(_WIN32)
#define DRACONIC_PLATFORM_WINDOWS 1
#elif defined(__linux__)
#define DRACONIC_PLATFORM_LINUX 1
#else
#error "Draconic: unsupported platform."
#endif

#if !defined(DRACONIC_PLATFORM_WINDOWS)
#define DRACONIC_PLATFORM_WINDOWS 0
#endif
#if !defined(DRACONIC_PLATFORM_LINUX)
#define DRACONIC_PLATFORM_LINUX 0
#endif
#if !defined(DRACONIC_PLATFORM_WEB)
#define DRACONIC_PLATFORM_WEB 0
#endif

// ---------------------------------------------------------------------------
// Architecture detection
// ---------------------------------------------------------------------------
#if defined(__x86_64__) || defined(_M_X64)
#define DRACONIC_ARCH_X64 1
#elif defined(__aarch64__) || defined(_M_ARM64)
#define DRACONIC_ARCH_ARM64 1
#elif defined(__wasm32__) || defined(__wasm64__)
#define DRACONIC_ARCH_WASM 1
#else
#error "Draconic: unsupported architecture."
#endif

#if !defined(DRACONIC_ARCH_X64)
#define DRACONIC_ARCH_X64 0
#endif
#if !defined(DRACONIC_ARCH_ARM64)
#define DRACONIC_ARCH_ARM64 0
#endif
#if !defined(DRACONIC_ARCH_WASM)
#define DRACONIC_ARCH_WASM 0
#endif

// Byte order. Both supported architectures run little-endian.
#define DRACONIC_LITTLE_ENDIAN 1

// ---------------------------------------------------------------------------
// Build configuration
//   DRACONIC_DEBUG    - asserts on, no/low optimization
//   DRACONIC_RELEASE  - optimized, asserts on
//   DRACONIC_SHIPPING - optimized, asserts compiled out
// Define exactly one via the build system; default to DRACONIC_DEBUG.
// ---------------------------------------------------------------------------
#if !defined(DRACONIC_DEBUG) && !defined(DRACONIC_RELEASE) && !defined(DRACONIC_SHIPPING)
#define DRACONIC_DEBUG 1
#endif

#if !defined(DRACONIC_DEBUG)
#define DRACONIC_DEBUG 0
#endif
#if !defined(DRACONIC_RELEASE)
#define DRACONIC_RELEASE 0
#endif
#if !defined(DRACONIC_SHIPPING)
#define DRACONIC_SHIPPING 0
#endif

// ---------------------------------------------------------------------------
// Attributes & ABI macros
// ---------------------------------------------------------------------------
#if DRACONIC_COMPILER_MSVC
#define DRACONIC_FORCEINLINE __forceinline
#define DRACONIC_NOINLINE __declspec(noinline)
#define DRACONIC_RESTRICT __restrict
#define DRACONIC_EXPORT __declspec(dllexport)
#define DRACONIC_IMPORT __declspec(dllimport)
#else
#define DRACONIC_FORCEINLINE inline __attribute__((always_inline))
#define DRACONIC_NOINLINE __attribute__((noinline))
#define DRACONIC_RESTRICT __restrict__
#define DRACONIC_EXPORT __attribute__((visibility("default")))
#define DRACONIC_IMPORT
#endif

// Foundation builds as a static library for now; DRACONIC_API is a no-op until we ship
// shared libraries. Plugins (see Library module) will flip this per target.
#define DRACONIC_API

// Expression-form branch hints are pass-throughs: Clang miscompiles
// __builtin_expect when it is reachable across C++ module units (it conflates
// call sites and reports an ambiguous call). For real hot paths, use the
// C++20 [[likely]] / [[unlikely]] attributes on if/switch statements instead.
#define DRACONIC_LIKELY(x) (x)
#define DRACONIC_UNLIKELY(x) (x)

#define DRACONIC_STRINGIFY_(x) #x
#define DRACONIC_STRINGIFY(x) DRACONIC_STRINGIFY_(x)

#endif // DRACONIC_FOUNDATION_PRELUDE_H
