// Draconic Foundation - :float3 partition
//
// Float3: 3D f32 vector - arithmetic, Dot/Cross/Length/Normalized, Min/Max,
// Lerp, component constants. Converts from Float2.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Debug/Assert.h"

export module draconic.foundation:float3;

import :base;
import :math;
import :float2;

export namespace draconic::foundation
{
    // =======================================================================
    // Float3
    // =======================================================================
    struct Float3
    {
        f32 x = 0.0f;
        f32 y = 0.0f;
        f32 z = 0.0f;

        constexpr Float3() noexcept = default;
        constexpr Float3(f32 inX, f32 inY, f32 inZ) noexcept : x(inX), y(inY), z(inZ) {}
        explicit constexpr Float3(f32 s) noexcept : x(s), y(s), z(s) {}
        constexpr Float3(Float2 xy, f32 inZ) noexcept : x(xy.x), y(xy.y), z(inZ) {}

        [[nodiscard]] constexpr f32& operator[](usize i) noexcept
        {
            DRACONIC_ASSERT(i < 3);
            return (&x)[i];
        }
        [[nodiscard]] constexpr f32 operator[](usize i) const noexcept
        {
            DRACONIC_ASSERT(i < 3);
            return (&x)[i];
        }

        constexpr Float3 operator-() const noexcept { return {-x, -y, -z}; }

        constexpr Float3& operator+=(Float3 r) noexcept
        {
            x += r.x;
            y += r.y;
            z += r.z;
            return *this;
        }
        constexpr Float3& operator-=(Float3 r) noexcept
        {
            x -= r.x;
            y -= r.y;
            z -= r.z;
            return *this;
        }
        constexpr Float3& operator*=(f32 s) noexcept
        {
            x *= s;
            y *= s;
            z *= s;
            return *this;
        }
        constexpr Float3& operator/=(f32 s) noexcept
        {
            x /= s;
            y /= s;
            z /= s;
            return *this;
        }

        static const Float3 Zero;
        static const Float3 One;
        static const Float3 UnitX;
        static const Float3 UnitY;
        static const Float3 UnitZ;
    };

    inline constexpr Float3 Float3::Zero{0.0f, 0.0f, 0.0f};
    inline constexpr Float3 Float3::One{1.0f, 1.0f, 1.0f};
    inline constexpr Float3 Float3::UnitX{1.0f, 0.0f, 0.0f};
    inline constexpr Float3 Float3::UnitY{0.0f, 1.0f, 0.0f};
    inline constexpr Float3 Float3::UnitZ{0.0f, 0.0f, 1.0f};

    [[nodiscard]] constexpr Float3 operator+(Float3 a, Float3 b) noexcept
    {
        return {a.x + b.x, a.y + b.y, a.z + b.z};
    }
    [[nodiscard]] constexpr Float3 operator-(Float3 a, Float3 b) noexcept
    {
        return {a.x - b.x, a.y - b.y, a.z - b.z};
    }
    [[nodiscard]] constexpr Float3 operator*(Float3 a, Float3 b) noexcept
    {
        return {a.x * b.x, a.y * b.y, a.z * b.z};
    }
    [[nodiscard]] constexpr Float3 operator*(Float3 v, f32 s) noexcept
    {
        return {v.x * s, v.y * s, v.z * s};
    }
    [[nodiscard]] constexpr Float3 operator*(f32 s, Float3 v) noexcept
    {
        return {v.x * s, v.y * s, v.z * s};
    }
    [[nodiscard]] constexpr Float3 operator/(Float3 v, f32 s) noexcept
    {
        return {v.x / s, v.y / s, v.z / s};
    }
    [[nodiscard]] constexpr Float3 operator/(Float3 a, Float3 b) noexcept
    {
        return {a.x / b.x, a.y / b.y, a.z / b.z};
    } // component-wise
    [[nodiscard]] constexpr bool operator==(Float3 a, Float3 b) noexcept
    {
        return a.x == b.x && a.y == b.y && a.z == b.z;
    }

    [[nodiscard]] constexpr f32 Dot(Float3 a, Float3 b) noexcept
    {
        return a.x * b.x + a.y * b.y + a.z * b.z;
    }

    [[nodiscard]] constexpr Float3 Cross(Float3 a, Float3 b) noexcept
    {
        return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
    }

    [[nodiscard]] constexpr f32 LengthSquared(Float3 v) noexcept { return Dot(v, v); }
    [[nodiscard]] inline f32 Length(Float3 v) noexcept { return Sqrt(LengthSquared(v)); }
    [[nodiscard]] inline f32 Distance(Float3 a, Float3 b) noexcept { return Length(b - a); }

    // Returns a unit vector, or Zero if the input is near-zero length.
    [[nodiscard]] inline Float3 Normalized(Float3 v) noexcept
    {
        const f32 lengthSq = LengthSquared(v);
        if (lengthSq <= kEpsilon * kEpsilon)
        {
            return Float3::Zero;
        }
        return v / Sqrt(lengthSq);
    }

    [[nodiscard]] constexpr Float3 Lerp(Float3 a, Float3 b, f32 t) noexcept
    {
        return a + (b - a) * t;
    }

    [[nodiscard]] constexpr Float3 Min(Float3 a, Float3 b) noexcept
    {
        return {a.x < b.x ? a.x : b.x, a.y < b.y ? a.y : b.y, a.z < b.z ? a.z : b.z};
    }
    [[nodiscard]] constexpr Float3 Max(Float3 a, Float3 b) noexcept
    {
        return {a.x > b.x ? a.x : b.x, a.y > b.y ? a.y : b.y, a.z > b.z ? a.z : b.z};
    }

    [[nodiscard]] inline bool NearlyEqual(Float3 a, Float3 b, f32 epsilon = kEpsilon) noexcept
    {
        return NearlyEqual(a.x, b.x, epsilon) && NearlyEqual(a.y, b.y, epsilon) &&
               NearlyEqual(a.z, b.z, epsilon);
    }
}
