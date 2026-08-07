// Draconic Foundation - :float4 partition
//
// Float4: 4D f32 vector - arithmetic, Dot/Length/Normalized, XYZ(), component
// constants. Converts from Float3.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Debug/Assert.h"

export module draconic.foundation:float4;

import :base;
import :math;
import :float3;

export namespace draconic::foundation
{
    // =======================================================================
    // Float4
    // =======================================================================
    struct Float4
    {
        f32 x = 0.0f;
        f32 y = 0.0f;
        f32 z = 0.0f;
        f32 w = 0.0f;

        constexpr Float4() noexcept = default;
        constexpr Float4(f32 inX, f32 inY, f32 inZ, f32 inW) noexcept
            : x(inX), y(inY), z(inZ), w(inW)
        {
        }
        explicit constexpr Float4(f32 s) noexcept : x(s), y(s), z(s), w(s) {}
        constexpr Float4(Float3 xyz, f32 inW) noexcept : x(xyz.x), y(xyz.y), z(xyz.z), w(inW) {}

        [[nodiscard]] constexpr f32& operator[](usize i) noexcept
        {
            DRACONIC_ASSERT(i < 4);
            return (&x)[i];
        }
        [[nodiscard]] constexpr f32 operator[](usize i) const noexcept
        {
            DRACONIC_ASSERT(i < 4);
            return (&x)[i];
        }

        [[nodiscard]] constexpr Float3 XYZ() const noexcept { return {x, y, z}; }

        constexpr Float4 operator-() const noexcept { return {-x, -y, -z, -w}; }

        constexpr Float4& operator+=(Float4 r) noexcept
        {
            x += r.x;
            y += r.y;
            z += r.z;
            w += r.w;
            return *this;
        }
        constexpr Float4& operator-=(Float4 r) noexcept
        {
            x -= r.x;
            y -= r.y;
            z -= r.z;
            w -= r.w;
            return *this;
        }
        constexpr Float4& operator*=(f32 s) noexcept
        {
            x *= s;
            y *= s;
            z *= s;
            w *= s;
            return *this;
        }

        static const Float4 Zero;
        static const Float4 One;
    };

    inline constexpr Float4 Float4::Zero{0.0f, 0.0f, 0.0f, 0.0f};
    inline constexpr Float4 Float4::One{1.0f, 1.0f, 1.0f, 1.0f};

    [[nodiscard]] constexpr Float4 operator+(Float4 a, Float4 b) noexcept
    {
        return {a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w};
    }
    [[nodiscard]] constexpr Float4 operator-(Float4 a, Float4 b) noexcept
    {
        return {a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w};
    }
    [[nodiscard]] constexpr Float4 operator*(Float4 v, f32 s) noexcept
    {
        return {v.x * s, v.y * s, v.z * s, v.w * s};
    }
    [[nodiscard]] constexpr Float4 operator*(f32 s, Float4 v) noexcept
    {
        return {v.x * s, v.y * s, v.z * s, v.w * s};
    }
    [[nodiscard]] constexpr Float4 Lerp(Float4 a, Float4 b, f32 t) noexcept
    {
        return a + (b - a) * t;
    }
    [[nodiscard]] constexpr bool operator==(Float4 a, Float4 b) noexcept
    {
        return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w;
    }

    [[nodiscard]] constexpr f32 Dot(Float4 a, Float4 b) noexcept
    {
        return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    }
    [[nodiscard]] constexpr f32 LengthSquared(Float4 v) noexcept { return Dot(v, v); }
    [[nodiscard]] inline f32 Length(Float4 v) noexcept { return Sqrt(LengthSquared(v)); }

    [[nodiscard]] inline Float4 Normalized(Float4 v) noexcept
    {
        const f32 lengthSq = LengthSquared(v);
        return (lengthSq <= kEpsilon * kEpsilon) ? Float4::Zero : v * (1.0f / Sqrt(lengthSq));
    }

    [[nodiscard]] inline bool NearlyEqual(Float4 a, Float4 b, f32 epsilon = kEpsilon) noexcept
    {
        return NearlyEqual(a.x, b.x, epsilon) && NearlyEqual(a.y, b.y, epsilon) &&
               NearlyEqual(a.z, b.z, epsilon) && NearlyEqual(a.w, b.w, epsilon);
    }
}
