// Draconic Foundation - SIMD backend selection (GMF-only header; not part of the module interface).
//
// Included in the global-module-fragment of the :simd* partitions to pick the register backend
// and pull in the intrinsic declarations. Macros do not cross the module boundary, so each simd
// unit includes this in its own GMF.
//
// Backends: SSE2 on x86/x64. Everything else (ARM/NEON, WASM) uses the scalar fallback, which is
// correct and portable - NEON/WASM-SIMD specializations are a future perf pass, not a correctness
// gap. The scalar path is also what the equivalence tests validate the SSE path against.
#pragma once

#if defined(__SSE2__) || defined(_M_X64) || defined(__x86_64__) ||                                 \
    (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#define DRACONIC_MATH_SSE 1
#include <emmintrin.h> // SSE2 (pulls in SSE/xmmintrin)
#else
#define DRACONIC_MATH_SSE 0
#endif
