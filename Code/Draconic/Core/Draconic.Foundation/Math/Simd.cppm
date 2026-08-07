// Draconic Foundation - :simd partition
//
// f32x4: a 4-lane float SIMD register abstraction, the substrate for the aligned Vector*/Matrix4
// compute types (:simd_vector, :simd_matrix). SSE2 backend on x86/x64, scalar fallback elsewhere
// (ARM/WASM) - the scalar path is correct and portable; NEON/WASM-SIMD are a later perf pass.
//
// This is plumbing: prefer Vector*/Matrix4 in application code. Everything is inline and branch-free
// per lane; the #if picks the backend at compile time.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Math/SimdConfig.h"

// SSE intrinsics are TU-local (header-provided) but safe in exported inline functions.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunknown-warning-option" // group below exists only in Clang 22+
#pragma clang diagnostic ignored "-WTU-local-entity-exposure"
#endif

export module draconic.foundation:simd;

import :base;

export namespace draconic::foundation::simd
{
    struct alignas(16) f32x4
    {
#if DRACONIC_MATH_SSE
        __m128 v;
#else
        f32 e[4];
#endif
    };

    // --- construction ------------------------------------------------------
    [[nodiscard]] inline f32x4 Set(f32 x, f32 y, f32 z, f32 w) noexcept
    {
#if DRACONIC_MATH_SSE
        return f32x4{_mm_set_ps(w, z, y, x)}; // note: _mm_set_ps takes high->low
#else
        return f32x4{{x, y, z, w}};
#endif
    }
    [[nodiscard]] inline f32x4 Splat(f32 s) noexcept
    {
#if DRACONIC_MATH_SSE
        return f32x4{_mm_set1_ps(s)};
#else
        return f32x4{{s, s, s, s}};
#endif
    }
    [[nodiscard]] inline f32x4 Zero() noexcept
    {
#if DRACONIC_MATH_SSE
        return f32x4{_mm_setzero_ps()};
#else
        return f32x4{{0.0f, 0.0f, 0.0f, 0.0f}};
#endif
    }

    // Writes the 4 lanes to out[0..3]. `out` must hold 4 floats (a scratch buffer at the
    // packed<->simd boundary; never point it straight at a Float3's 12 bytes).
    inline void Store(f32x4 a, f32 out[4]) noexcept
    {
#if DRACONIC_MATH_SSE
        _mm_storeu_ps(out, a.v);
#else
        out[0] = a.e[0];
        out[1] = a.e[1];
        out[2] = a.e[2];
        out[3] = a.e[3];
#endif
    }

    // --- arithmetic --------------------------------------------------------
    [[nodiscard]] inline f32x4 Add(f32x4 a, f32x4 b) noexcept
    {
#if DRACONIC_MATH_SSE
        return f32x4{_mm_add_ps(a.v, b.v)};
#else
        return f32x4{{a.e[0] + b.e[0], a.e[1] + b.e[1], a.e[2] + b.e[2], a.e[3] + b.e[3]}};
#endif
    }
    [[nodiscard]] inline f32x4 Sub(f32x4 a, f32x4 b) noexcept
    {
#if DRACONIC_MATH_SSE
        return f32x4{_mm_sub_ps(a.v, b.v)};
#else
        return f32x4{{a.e[0] - b.e[0], a.e[1] - b.e[1], a.e[2] - b.e[2], a.e[3] - b.e[3]}};
#endif
    }
    [[nodiscard]] inline f32x4 Mul(f32x4 a, f32x4 b) noexcept
    {
#if DRACONIC_MATH_SSE
        return f32x4{_mm_mul_ps(a.v, b.v)};
#else
        return f32x4{{a.e[0] * b.e[0], a.e[1] * b.e[1], a.e[2] * b.e[2], a.e[3] * b.e[3]}};
#endif
    }
    [[nodiscard]] inline f32x4 Div(f32x4 a, f32x4 b) noexcept
    {
#if DRACONIC_MATH_SSE
        return f32x4{_mm_div_ps(a.v, b.v)};
#else
        return f32x4{{a.e[0] / b.e[0], a.e[1] / b.e[1], a.e[2] / b.e[2], a.e[3] / b.e[3]}};
#endif
    }
    [[nodiscard]] inline f32x4 Mul(f32x4 a, f32 s) noexcept { return Mul(a, Splat(s)); }
    [[nodiscard]] inline f32x4 Neg(f32x4 a) noexcept { return Sub(Zero(), a); }

    [[nodiscard]] inline f32x4 Min(f32x4 a, f32x4 b) noexcept
    {
#if DRACONIC_MATH_SSE
        return f32x4{_mm_min_ps(a.v, b.v)};
#else
        return f32x4{{a.e[0] < b.e[0] ? a.e[0] : b.e[0], a.e[1] < b.e[1] ? a.e[1] : b.e[1],
                      a.e[2] < b.e[2] ? a.e[2] : b.e[2], a.e[3] < b.e[3] ? a.e[3] : b.e[3]}};
#endif
    }
    [[nodiscard]] inline f32x4 Max(f32x4 a, f32x4 b) noexcept
    {
#if DRACONIC_MATH_SSE
        return f32x4{_mm_max_ps(a.v, b.v)};
#else
        return f32x4{{a.e[0] > b.e[0] ? a.e[0] : b.e[0], a.e[1] > b.e[1] ? a.e[1] : b.e[1],
                      a.e[2] > b.e[2] ? a.e[2] : b.e[2], a.e[3] > b.e[3] ? a.e[3] : b.e[3]}};
#endif
    }

    // Sum of the four lanes (SSE2-only horizontal add).
    [[nodiscard]] inline f32 HSum4(f32x4 a) noexcept
    {
#if DRACONIC_MATH_SSE
        __m128 v = a.v;
        __m128 shuf = _mm_shuffle_ps(v, v, _MM_SHUFFLE(2, 3, 0, 1)); // (y, x, w, z)
        __m128 sums = _mm_add_ps(v, shuf);                           // (x+y, y+x, z+w, w+z)
        shuf = _mm_movehl_ps(shuf, sums);                            // (z+w, w+z, ...)
        sums = _mm_add_ss(sums, shuf);                               // (x+y)+(z+w)
        return _mm_cvtss_f32(sums);
#else
        return a.e[0] + a.e[1] + a.e[2] + a.e[3];
#endif
    }

    // Zero the w lane (keeps the Float3-in-a-register invariant after ops that touch w).
    [[nodiscard]] inline f32x4 ZeroW(f32x4 a) noexcept
    {
#if DRACONIC_MATH_SSE
        const __m128 mask = _mm_castsi128_ps(_mm_set_epi32(0, -1, -1, -1));
        return f32x4{_mm_and_ps(a.v, mask)};
#else
        return f32x4{{a.e[0], a.e[1], a.e[2], 0.0f}};
#endif
    }

    // Lane permutation: result lane k = a[i_k]. (SSE takes the selector high->low.)
    template <int i0, int i1, int i2, int i3>
    [[nodiscard]] inline f32x4 Shuffle(f32x4 a) noexcept
    {
#if DRACONIC_MATH_SSE
        return f32x4{_mm_shuffle_ps(a.v, a.v, _MM_SHUFFLE(i3, i2, i1, i0))};
#else
        return f32x4{{a.e[i0], a.e[i1], a.e[i2], a.e[i3]}};
#endif
    }
    [[nodiscard]] inline f32x4 SplatX(f32x4 a) noexcept { return Shuffle<0, 0, 0, 0>(a); }
    [[nodiscard]] inline f32x4 SplatY(f32x4 a) noexcept { return Shuffle<1, 1, 1, 1>(a); }
    [[nodiscard]] inline f32x4 SplatZ(f32x4 a) noexcept { return Shuffle<2, 2, 2, 2>(a); }
    [[nodiscard]] inline f32x4 SplatW(f32x4 a) noexcept { return Shuffle<3, 3, 3, 3>(a); }
}
