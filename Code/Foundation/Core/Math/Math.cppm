// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Core - :math partition (scalar constants & functions)
//
// Scalar math foundation: constants (kPi, kEpsilon, ...) and f32 functions
// (Abs/Sqrt/Sin/.../Lerp/NearlyEqual). Packed vector/matrix types live in
// :float2/:float3/:float4/:float3x3/:float4x4; SIMD (aligned) types in
// :simd_vector/:simd_matrix. Conventions:
// row-major matrices, row vectors, XNA-style.

module;
#include "Core/Prelude.h"
#include <cmath>

export module foundation.core:math;

import :base;

export namespace foundation::core
{
    // =======================================================================
    // Constants & scalar functions
    // =======================================================================
    inline constexpr f32 kPi = 3.14159265358979323846f;
    inline constexpr f32 kTwoPi = 2.0f * kPi;
    inline constexpr f32 kHalfPi = 0.5f * kPi;
    inline constexpr f32 kInvPi = 1.0f / kPi;
    inline constexpr f32 kDegToRad = kPi / 180.0f;
    inline constexpr f32 kRadToDeg = 180.0f / kPi;
    inline constexpr f32 kEpsilon = 1.0e-6f;
    inline constexpr f32 kFloatMax = 3.402823466e38f;

    // Overloaded so integer and double arguments do not silently narrow through the f32
    // version (MSVC C4244). `Abs(a - b)` over two integers, common in pixel and index code,
    // stays integral and exact.
    [[nodiscard]] constexpr f32 Abs(f32 x) noexcept { return x < 0.0f ? -x : x; }
    [[nodiscard]] constexpr f64 Abs(f64 x) noexcept { return x < 0.0 ? -x : x; }
    [[nodiscard]] constexpr i32 Abs(i32 x) noexcept { return x < 0 ? -x : x; }
    [[nodiscard]] constexpr i64 Abs(i64 x) noexcept { return x < 0 ? -x : x; }
    [[nodiscard]] inline f32 Sqrt(f32 x) noexcept { return std::sqrt(x); }
    [[nodiscard]] inline f32 Sin(f32 x) noexcept { return std::sin(x); }
    [[nodiscard]] inline f32 Cos(f32 x) noexcept { return std::cos(x); }
    [[nodiscard]] inline f32 Tan(f32 x) noexcept { return std::tan(x); }
    [[nodiscard]] inline f32 Asin(f32 x) noexcept { return std::asin(x); }
    [[nodiscard]] inline f32 Acos(f32 x) noexcept { return std::acos(x); }
    [[nodiscard]] inline f32 Atan2(f32 y, f32 x) noexcept { return std::atan2(y, x); }
    [[nodiscard]] inline f32 Floor(f32 x) noexcept { return std::floor(x); }
    [[nodiscard]] inline f32 Ceil(f32 x) noexcept { return std::ceil(x); }
    [[nodiscard]] inline f32 Round(f32 x) noexcept
    {
        return std::round(x);
    } // round half away from zero
    [[nodiscard]] inline f32 Pow(f32 base, f32 exp) noexcept { return std::pow(base, exp); }
    [[nodiscard]] inline f32 Log(f32 x) noexcept { return std::log(x); } // natural log
    [[nodiscard]] inline f32 Exp(f32 x) noexcept { return std::exp(x); }

    [[nodiscard]] constexpr f32 DegreesToRadians(f32 degrees) noexcept
    {
        return degrees * kDegToRad;
    }
    [[nodiscard]] constexpr f32 RadiansToDegrees(f32 radians) noexcept
    {
        return radians * kRadToDeg;
    }

    [[nodiscard]] constexpr f32 Lerp(f32 a, f32 b, f32 t) noexcept { return a + (b - a) * t; }

    // By value, for the script Math: Base's Min/Max/Clamp templates return a reference, which the
    // reflected statics cannot carry. An exact f32 match, so C++ callers pick these too.
    [[nodiscard]] constexpr f32 Min(f32 a, f32 b) noexcept { return b < a ? b : a; }
    [[nodiscard]] constexpr f32 Max(f32 a, f32 b) noexcept { return a < b ? b : a; }
    [[nodiscard]] constexpr f32 Clamp(f32 x, f32 lo, f32 hi) noexcept
    {
        return x < lo ? lo : (hi < x ? hi : x);
    }

    [[nodiscard]] inline bool NearlyEqual(f32 a, f32 b, f32 epsilon = kEpsilon) noexcept
    {
        return Abs(a - b) <= epsilon;
    }

    [[nodiscard]] inline bool NearlyZero(f32 x, f32 epsilon = kEpsilon) noexcept
    {
        return Abs(x) <= epsilon;
    }

    // Script-facing anchor: the free scalar functions above are reflected as statics on this type
    // (CoreReflectionImpl), so scripts call `Math.Sin(x)`, `Math::Atan2(y, x)` and so on. It is
    // empty: it only gives the reflected math functions a home, since a namespace cannot be reflected.
    struct Math
    {
    };
}
