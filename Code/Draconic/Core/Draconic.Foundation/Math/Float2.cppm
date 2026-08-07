// Draconic Foundation - :float2 partition
//
// Float2: 2D f32 vector - arithmetic, Dot/Length/Normalized, component
// constants. Built on the :math scalar functions.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Debug/Assert.h"

export module draconic.foundation:float2;

import :base;
import :math;

export namespace draconic::foundation
{
    // =======================================================================
    // Float2
    // =======================================================================
    struct Float2
    {
        f32 x = 0.0f;
        f32 y = 0.0f;

        constexpr Float2() noexcept = default;
        constexpr Float2(f32 inX, f32 inY) noexcept : x(inX), y(inY) {}
        explicit constexpr Float2(f32 s) noexcept : x(s), y(s) {}

        [[nodiscard]] constexpr f32& operator[](usize i) noexcept
        {
            DRACONIC_ASSERT(i < 2);
            return (&x)[i];
        }
        [[nodiscard]] constexpr f32 operator[](usize i) const noexcept
        {
            DRACONIC_ASSERT(i < 2);
            return (&x)[i];
        }

        constexpr Float2 operator-() const noexcept { return {-x, -y}; }

        constexpr Float2& operator+=(Float2 r) noexcept
        {
            x += r.x;
            y += r.y;
            return *this;
        }
        constexpr Float2& operator-=(Float2 r) noexcept
        {
            x -= r.x;
            y -= r.y;
            return *this;
        }
        constexpr Float2& operator*=(f32 s) noexcept
        {
            x *= s;
            y *= s;
            return *this;
        }
        constexpr Float2& operator/=(f32 s) noexcept
        {
            x /= s;
            y /= s;
            return *this;
        }

        static const Float2 Zero;
        static const Float2 One;
        static const Float2 UnitX;
        static const Float2 UnitY;
    };

    inline constexpr Float2 Float2::Zero{0.0f, 0.0f};
    inline constexpr Float2 Float2::One{1.0f, 1.0f};
    inline constexpr Float2 Float2::UnitX{1.0f, 0.0f};
    inline constexpr Float2 Float2::UnitY{0.0f, 1.0f};

    [[nodiscard]] constexpr Float2 operator+(Float2 a, Float2 b) noexcept
    {
        return {a.x + b.x, a.y + b.y};
    }
    [[nodiscard]] constexpr Float2 operator-(Float2 a, Float2 b) noexcept
    {
        return {a.x - b.x, a.y - b.y};
    }
    [[nodiscard]] constexpr Float2 operator*(Float2 a, Float2 b) noexcept
    {
        return {a.x * b.x, a.y * b.y};
    }
    [[nodiscard]] constexpr Float2 operator*(Float2 v, f32 s) noexcept
    {
        return {v.x * s, v.y * s};
    }
    [[nodiscard]] constexpr Float2 operator*(f32 s, Float2 v) noexcept
    {
        return {v.x * s, v.y * s};
    }
    [[nodiscard]] constexpr Float2 operator/(Float2 v, f32 s) noexcept
    {
        return {v.x / s, v.y / s};
    }
    [[nodiscard]] constexpr bool operator==(Float2 a, Float2 b) noexcept
    {
        return a.x == b.x && a.y == b.y;
    }

    [[nodiscard]] constexpr f32 Dot(Float2 a, Float2 b) noexcept { return a.x * b.x + a.y * b.y; }
    [[nodiscard]] constexpr f32 LengthSquared(Float2 v) noexcept { return Dot(v, v); }
    [[nodiscard]] inline f32 Length(Float2 v) noexcept { return Sqrt(LengthSquared(v)); }

    [[nodiscard]] inline Float2 Normalized(Float2 v) noexcept
    {
        const f32 lengthSq = LengthSquared(v);
        return (lengthSq <= kEpsilon * kEpsilon) ? Float2::Zero : v / Sqrt(lengthSq);
    }

    [[nodiscard]] constexpr f32 DistanceSquared(Float2 a, Float2 b) noexcept
    {
        return LengthSquared(b - a);
    }
    [[nodiscard]] inline f32 Distance(Float2 a, Float2 b) noexcept { return Length(b - a); }

    [[nodiscard]] constexpr Float2 Lerp(Float2 a, Float2 b, f32 t) noexcept
    {
        return a + (b - a) * t;
    }

    [[nodiscard]] inline bool NearlyEqual(Float2 a, Float2 b, f32 epsilon = kEpsilon) noexcept
    {
        return NearlyEqual(a.x, b.x, epsilon) && NearlyEqual(a.y, b.y, epsilon);
    }
}
