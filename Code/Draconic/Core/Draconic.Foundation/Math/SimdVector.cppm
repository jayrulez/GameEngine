// Draconic Foundation - :simd_vector partition
//
// Vector2/3/4: aligned (16-byte) SIMD compute types, backed by simd::f32x4. These are the types to
// reach for in hot CPU math (transforms, culling, animation). They are NOT storage types - store data
// in the packed Float2/3/4 (which match shader/vertex/cbuffer layout) and Load into a Vector* to
// compute, then extract with ToFloatN(). Conversions are explicit on purpose: crossing packed<->simd
// is a deliberate boundary, not an accident.
//
// API mirrors the packed types (arithmetic, Dot, Cross, Length, Normalized, Lerp, Min, Max) so a
// hot-path migration is mostly a type swap plus Load/Store at the edges.
//
// Vector3 keeps its w lane at 0 by invariant, so Dot/Length ignore w naturally.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Math/SimdConfig.h"

export module draconic.foundation:simd_vector;

import :base;
import :math; // Sqrt, kEpsilon
import :simd;
import :float2;
import :float3;
import :float4;

export namespace draconic::foundation
{
    // =======================================================================
    // Vector4
    // =======================================================================
    struct alignas(16) Vector4
    {
        simd::f32x4 r;

        Vector4() noexcept : r(simd::Zero()) {}
        Vector4(f32 x, f32 y, f32 z, f32 w) noexcept : r(simd::Set(x, y, z, w)) {}
        explicit Vector4(f32 s) noexcept : r(simd::Splat(s)) {}
        explicit Vector4(simd::f32x4 v) noexcept : r(v) {}
        explicit Vector4(const Float4& f) noexcept : r(simd::Set(f.x, f.y, f.z, f.w)) {}

        [[nodiscard]] Float4 ToFloat4() const noexcept
        {
            f32 b[4];
            simd::Store(r, b);
            return {b[0], b[1], b[2], b[3]};
        }

        Vector4& operator+=(Vector4 o) noexcept
        {
            r = simd::Add(r, o.r);
            return *this;
        }
        Vector4& operator-=(Vector4 o) noexcept
        {
            r = simd::Sub(r, o.r);
            return *this;
        }
        Vector4& operator*=(f32 s) noexcept
        {
            r = simd::Mul(r, s);
            return *this;
        }
    };

    [[nodiscard]] inline Vector4 operator+(Vector4 a, Vector4 b) noexcept
    {
        return Vector4(simd::Add(a.r, b.r));
    }
    [[nodiscard]] inline Vector4 operator-(Vector4 a, Vector4 b) noexcept
    {
        return Vector4(simd::Sub(a.r, b.r));
    }
    [[nodiscard]] inline Vector4 operator-(Vector4 a) noexcept { return Vector4(simd::Neg(a.r)); }
    [[nodiscard]] inline Vector4 operator*(Vector4 a, Vector4 b) noexcept
    {
        return Vector4(simd::Mul(a.r, b.r));
    } // component-wise
    [[nodiscard]] inline Vector4 operator*(Vector4 v, f32 s) noexcept
    {
        return Vector4(simd::Mul(v.r, s));
    }
    [[nodiscard]] inline Vector4 operator*(f32 s, Vector4 v) noexcept
    {
        return Vector4(simd::Mul(v.r, s));
    }
    [[nodiscard]] inline Vector4 operator/(Vector4 v, f32 s) noexcept
    {
        return Vector4(simd::Div(v.r, simd::Splat(s)));
    }

    [[nodiscard]] inline f32 Dot(Vector4 a, Vector4 b) noexcept
    {
        return simd::HSum4(simd::Mul(a.r, b.r));
    }
    [[nodiscard]] inline f32 LengthSquared(Vector4 v) noexcept { return Dot(v, v); }
    [[nodiscard]] inline f32 Length(Vector4 v) noexcept { return Sqrt(LengthSquared(v)); }
    [[nodiscard]] inline Vector4 Normalized(Vector4 v) noexcept
    {
        const f32 lengthSq = LengthSquared(v);
        if (lengthSq <= kEpsilon * kEpsilon)
        {
            return Vector4();
        }
        return Vector4(simd::Mul(v.r, 1.0f / Sqrt(lengthSq)));
    }
    [[nodiscard]] inline Vector4 Lerp(Vector4 a, Vector4 b, f32 t) noexcept
    {
        return a + (b - a) * t;
    }
    [[nodiscard]] inline Vector4 Min(Vector4 a, Vector4 b) noexcept
    {
        return Vector4(simd::Min(a.r, b.r));
    }
    [[nodiscard]] inline Vector4 Max(Vector4 a, Vector4 b) noexcept
    {
        return Vector4(simd::Max(a.r, b.r));
    }

    // =======================================================================
    // Vector3 (w lane held at 0)
    // =======================================================================
    struct alignas(16) Vector3
    {
        simd::f32x4 r;

        Vector3() noexcept : r(simd::Zero()) {}
        Vector3(f32 x, f32 y, f32 z) noexcept : r(simd::Set(x, y, z, 0.0f)) {}
        explicit Vector3(f32 s) noexcept : r(simd::Set(s, s, s, 0.0f)) {}
        // Wraps a register directly; forces w=0 to preserve the Vector3 invariant.
        explicit Vector3(simd::f32x4 v) noexcept : r(simd::ZeroW(v)) {}
        explicit Vector3(const Float3& f) noexcept : r(simd::Set(f.x, f.y, f.z, 0.0f)) {}

        [[nodiscard]] Float3 ToFloat3() const noexcept
        {
            f32 b[4];
            simd::Store(r, b);
            return {b[0], b[1], b[2]};
        }

        Vector3& operator+=(Vector3 o) noexcept
        {
            r = simd::Add(r, o.r);
            return *this;
        }
        Vector3& operator-=(Vector3 o) noexcept
        {
            r = simd::Sub(r, o.r);
            return *this;
        }
        Vector3& operator*=(f32 s) noexcept
        {
            r = simd::Mul(r, s);
            return *this;
        }
    };

    [[nodiscard]] inline Vector3 operator+(Vector3 a, Vector3 b) noexcept
    {
        return Vector3(simd::Add(a.r, b.r));
    }
    [[nodiscard]] inline Vector3 operator-(Vector3 a, Vector3 b) noexcept
    {
        return Vector3(simd::Sub(a.r, b.r));
    }
    [[nodiscard]] inline Vector3 operator-(Vector3 a) noexcept { return Vector3(simd::Neg(a.r)); }
    [[nodiscard]] inline Vector3 operator*(Vector3 a, Vector3 b) noexcept
    {
        return Vector3(simd::Mul(a.r, b.r));
    } // component-wise
    [[nodiscard]] inline Vector3 operator*(Vector3 v, f32 s) noexcept
    {
        return Vector3(simd::Mul(v.r, s));
    }
    [[nodiscard]] inline Vector3 operator*(f32 s, Vector3 v) noexcept
    {
        return Vector3(simd::Mul(v.r, s));
    }
    [[nodiscard]] inline Vector3 operator/(Vector3 v, f32 s) noexcept
    {
        return Vector3(simd::Div(v.r, simd::Splat(s)));
    }

    [[nodiscard]] inline f32 Dot(Vector3 a, Vector3 b) noexcept
    {
        return simd::HSum4(simd::Mul(a.r, b.r));
    } // w=0, so xyz only
    [[nodiscard]] inline Vector3 Cross(Vector3 a, Vector3 b) noexcept
    {
        const simd::f32x4 aYzx = simd::Shuffle<1, 2, 0, 3>(a.r);
        const simd::f32x4 aZxy = simd::Shuffle<2, 0, 1, 3>(a.r);
        const simd::f32x4 bYzx = simd::Shuffle<1, 2, 0, 3>(b.r);
        const simd::f32x4 bZxy = simd::Shuffle<2, 0, 1, 3>(b.r);
        return Vector3(simd::Sub(simd::Mul(aYzx, bZxy), simd::Mul(aZxy, bYzx)));
    }
    [[nodiscard]] inline f32 LengthSquared(Vector3 v) noexcept { return Dot(v, v); }
    [[nodiscard]] inline f32 Length(Vector3 v) noexcept { return Sqrt(LengthSquared(v)); }
    [[nodiscard]] inline f32 Distance(Vector3 a, Vector3 b) noexcept { return Length(b - a); }
    [[nodiscard]] inline Vector3 Normalized(Vector3 v) noexcept
    {
        const f32 lengthSq = LengthSquared(v);
        if (lengthSq <= kEpsilon * kEpsilon)
        {
            return Vector3();
        }
        return Vector3(simd::Mul(v.r, 1.0f / Sqrt(lengthSq)));
    }
    [[nodiscard]] inline Vector3 Lerp(Vector3 a, Vector3 b, f32 t) noexcept
    {
        return a + (b - a) * t;
    }
    [[nodiscard]] inline Vector3 Min(Vector3 a, Vector3 b) noexcept
    {
        return Vector3(simd::Min(a.r, b.r));
    }
    [[nodiscard]] inline Vector3 Max(Vector3 a, Vector3 b) noexcept
    {
        return Vector3(simd::Max(a.r, b.r));
    }

    // =======================================================================
    // Vector2 (z, w lanes held at 0)
    // =======================================================================
    struct alignas(16) Vector2
    {
        simd::f32x4 r;

        Vector2() noexcept : r(simd::Zero()) {}
        Vector2(f32 x, f32 y) noexcept : r(simd::Set(x, y, 0.0f, 0.0f)) {}
        explicit Vector2(f32 s) noexcept : r(simd::Set(s, s, 0.0f, 0.0f)) {}
        explicit Vector2(simd::f32x4 v) noexcept : r(v) {}
        explicit Vector2(const Float2& f) noexcept : r(simd::Set(f.x, f.y, 0.0f, 0.0f)) {}

        [[nodiscard]] Float2 ToFloat2() const noexcept
        {
            f32 b[4];
            simd::Store(r, b);
            return {b[0], b[1]};
        }

        Vector2& operator+=(Vector2 o) noexcept
        {
            r = simd::Add(r, o.r);
            return *this;
        }
        Vector2& operator-=(Vector2 o) noexcept
        {
            r = simd::Sub(r, o.r);
            return *this;
        }
        Vector2& operator*=(f32 s) noexcept
        {
            r = simd::Mul(r, s);
            return *this;
        }
    };

    [[nodiscard]] inline Vector2 operator+(Vector2 a, Vector2 b) noexcept
    {
        return Vector2(simd::Add(a.r, b.r));
    }
    [[nodiscard]] inline Vector2 operator-(Vector2 a, Vector2 b) noexcept
    {
        return Vector2(simd::Sub(a.r, b.r));
    }
    [[nodiscard]] inline Vector2 operator-(Vector2 a) noexcept { return Vector2(simd::Neg(a.r)); }
    [[nodiscard]] inline Vector2 operator*(Vector2 a, Vector2 b) noexcept
    {
        return Vector2(simd::Mul(a.r, b.r));
    } // component-wise
    [[nodiscard]] inline Vector2 operator*(Vector2 v, f32 s) noexcept
    {
        return Vector2(simd::Mul(v.r, s));
    }
    [[nodiscard]] inline Vector2 operator*(f32 s, Vector2 v) noexcept
    {
        return Vector2(simd::Mul(v.r, s));
    }
    [[nodiscard]] inline Vector2 operator/(Vector2 v, f32 s) noexcept
    {
        return Vector2(simd::Div(v.r, simd::Splat(s)));
    }

    [[nodiscard]] inline f32 Dot(Vector2 a, Vector2 b) noexcept
    {
        return simd::HSum4(simd::Mul(a.r, b.r));
    } // z=w=0
    [[nodiscard]] inline f32 LengthSquared(Vector2 v) noexcept { return Dot(v, v); }
    [[nodiscard]] inline f32 Length(Vector2 v) noexcept { return Sqrt(LengthSquared(v)); }
    [[nodiscard]] inline Vector2 Normalized(Vector2 v) noexcept
    {
        const f32 lengthSq = LengthSquared(v);
        if (lengthSq <= kEpsilon * kEpsilon)
        {
            return Vector2();
        }
        return Vector2(simd::Mul(v.r, 1.0f / Sqrt(lengthSq)));
    }
    [[nodiscard]] inline Vector2 Lerp(Vector2 a, Vector2 b, f32 t) noexcept
    {
        return a + (b - a) * t;
    }
    [[nodiscard]] inline Vector2 Min(Vector2 a, Vector2 b) noexcept
    {
        return Vector2(simd::Min(a.r, b.r));
    }
    [[nodiscard]] inline Vector2 Max(Vector2 a, Vector2 b) noexcept
    {
        return Vector2(simd::Max(a.r, b.r));
    }
}
