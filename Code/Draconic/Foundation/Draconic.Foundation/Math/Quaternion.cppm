// Draconic Foundation - :quaternion partition
// Quaternion: unit quaternion rotation - FromAxisAngle, Hamilton product,
// Conjugate/Dot/Normalized/Slerp, RotateVector, and RotationMatrix (-> Float4x4).
//
// Conventions: row-major storage m[row][col];
// row vectors (v' = v * M); composition left-to-right; XNA-style right-handed
// projections, NDC depth [0,1]; translation in the last row.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Debug/Assert.h"

export module draconic.foundation:quaternion;

import :base;
import :math;
import :float3;
import :float4x4;

export namespace draconic::foundation
{
    // =======================================================================
    // Quaternion - unit quaternion rotation (x, y, z, w).
    // =======================================================================
    struct Quaternion
    {
        f32 x = 0.0f;
        f32 y = 0.0f;
        f32 z = 0.0f;
        f32 w = 1.0f;

        constexpr Quaternion() noexcept = default;
        constexpr Quaternion(f32 inX, f32 inY, f32 inZ, f32 inW) noexcept
            : x(inX), y(inY), z(inZ), w(inW)
        {
        }

        [[nodiscard]] static Quaternion FromAxisAngle(Float3 axis, f32 radians) noexcept
        {
            const f32 half = radians * 0.5f;
            const f32 s = Sin(half);
            const Float3 a = Normalized(axis);
            return Quaternion{a.x * s, a.y * s, a.z * s, Cos(half)};
        }

        static const Quaternion Identity;
    };

    inline constexpr Quaternion Quaternion::Identity{0.0f, 0.0f, 0.0f, 1.0f};

    // Hamilton product: applies `b` then `a` to a vector.
    [[nodiscard]] constexpr Quaternion operator*(Quaternion a, Quaternion b) noexcept
    {
        return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
                a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
                a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
    }

    [[nodiscard]] constexpr Quaternion Conjugate(Quaternion q) noexcept
    {
        return {-q.x, -q.y, -q.z, q.w};
    }
    [[nodiscard]] constexpr f32 Dot(Quaternion a, Quaternion b) noexcept
    {
        return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    }

    // General inverse (= conjugate / |q|^2). For unit quaternions this equals the conjugate.
    [[nodiscard]] inline Quaternion Inverse(Quaternion q) noexcept
    {
        const f32 lengthSq = Dot(q, q);
        if (lengthSq <= kEpsilon * kEpsilon)
        {
            return Quaternion::Identity;
        }
        const f32 inv = 1.0f / lengthSq;
        return {-q.x * inv, -q.y * inv, -q.z * inv, q.w * inv};
    }

    [[nodiscard]] inline Quaternion Normalized(Quaternion q) noexcept
    {
        const f32 lengthSq = Dot(q, q);
        if (lengthSq <= kEpsilon * kEpsilon)
        {
            return Quaternion::Identity;
        }
        const f32 inv = 1.0f / Sqrt(lengthSq);
        return {q.x * inv, q.y * inv, q.z * inv, q.w * inv};
    }

    [[nodiscard]] constexpr Float3 RotateVector(Quaternion q, Float3 v) noexcept
    {
        const Float3 u{q.x, q.y, q.z};
        const f32 s = q.w;
        return u * (2.0f * Dot(u, v)) + v * (s * s - Dot(u, u)) + Cross(u, v) * (2.0f * s);
    }

    [[nodiscard]] inline bool NearlyEqual(Quaternion a, Quaternion b,
                                          f32 epsilon = kEpsilon) noexcept
    {
        return NearlyEqual(a.x, b.x, epsilon) && NearlyEqual(a.y, b.y, epsilon) &&
               NearlyEqual(a.z, b.z, epsilon) && NearlyEqual(a.w, b.w, epsilon);
    }

    // Spherical linear interpolation along the shortest arc; result is unit.
    [[nodiscard]] inline Quaternion Slerp(Quaternion a, Quaternion b, f32 t) noexcept
    {
        f32 cosTheta = Dot(a, b);
        if (cosTheta < 0.0f) // shortest path
        {
            b = Quaternion{-b.x, -b.y, -b.z, -b.w};
            cosTheta = -cosTheta;
        }

        if (cosTheta > 0.9995f) // nearly parallel - lerp + normalize
        {
            return Normalized(Quaternion{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t,
                                         a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t});
        }

        const f32 theta0 = Acos(cosTheta);
        const f32 theta = theta0 * t;
        const f32 sinTheta = Sin(theta);
        const f32 sinTheta0 = Sin(theta0);
        const f32 s1 = sinTheta / sinTheta0;
        const f32 s0 = Cos(theta) - cosTheta * s1;
        return Quaternion{a.x * s0 + b.x * s1, a.y * s0 + b.y * s1, a.z * s0 + b.z * s1,
                          a.w * s0 + b.w * s1};
    }

    // Rotation matrix for a unit quaternion (row-vector convention, XNA layout).
    [[nodiscard]] constexpr Float4x4 RotationMatrix(Quaternion q) noexcept
    {
        const f32 xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
        const f32 xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
        const f32 wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
        return Float4x4{{{1.0f - 2.0f * (yy + zz), 2.0f * (xy + wz), 2.0f * (xz - wy), 0.0f},
                         {2.0f * (xy - wz), 1.0f - 2.0f * (xx + zz), 2.0f * (yz + wx), 0.0f},
                         {2.0f * (xz + wy), 2.0f * (yz - wx), 1.0f - 2.0f * (xx + yy), 0.0f},
                         {0.0f, 0.0f, 0.0f, 1.0f}}};
    }

    // Yaw (Y), pitch (X), roll (Z) in radians -> quaternion (XNA convention: q = qY * qX * qZ).
    // Ported from Sedulous Quaternion.CreateFromYawPitchRoll.
    [[nodiscard]] inline Quaternion FromYawPitchRoll(f32 yaw, f32 pitch, f32 roll) noexcept
    {
        const f32 sr = Sin(roll * 0.5f), cr = Cos(roll * 0.5f);
        const f32 sp = Sin(pitch * 0.5f), cp = Cos(pitch * 0.5f);
        const f32 sy = Sin(yaw * 0.5f), cy = Cos(yaw * 0.5f);
        return Quaternion{cy * sp * cr + sy * cp * sr, sy * cp * cr - cy * sp * sr,
                          cy * cp * sr - sy * sp * cr, cy * cp * cr + sy * sp * sr};
    }

    // FromYawPitchRoll's inverse (pitch clamped to +-90deg; at the gimbal poles yaw/roll are not
    // unique). The editor's rotation-as-euler display seam.
    inline void ToYawPitchRoll(Quaternion q, f32& yaw, f32& pitch, f32& roll) noexcept
    {
        pitch = Asin(Clamp(2.0f * (q.w * q.x - q.y * q.z), -1.0f, 1.0f));
        yaw = Atan2(2.0f * (q.w * q.y + q.x * q.z), 1.0f - 2.0f * (q.x * q.x + q.y * q.y));
        roll = Atan2(2.0f * (q.w * q.z + q.x * q.y), 1.0f - 2.0f * (q.x * q.x + q.z * q.z));
    }

    // RotationMatrix's inverse: the unit quaternion of a pure rotation matrix (row-vector, XNA
    // layout). Shepperd's method over the trace / dominant diagonal. Ported from Sedulous
    // Quaternion.CreateFromRotationMatrix (Mrc -> m[r-1][c-1]).
    [[nodiscard]] inline Quaternion QuaternionFromRotationMatrix(const Float4x4& m) noexcept
    {
        const f32 trace = m.m[0][0] + m.m[1][1] + m.m[2][2];
        Quaternion result;
        if (trace > 0.0f)
        {
            f32 s = Sqrt(trace + 1.0f);
            result.w = s * 0.5f;
            s = 0.5f / s;
            result.x = (m.m[1][2] - m.m[2][1]) * s;
            result.y = (m.m[2][0] - m.m[0][2]) * s;
            result.z = (m.m[0][1] - m.m[1][0]) * s;
        }
        else if (m.m[0][0] >= m.m[1][1] && m.m[0][0] >= m.m[2][2])
        {
            const f32 s = Sqrt(1.0f + m.m[0][0] - m.m[1][1] - m.m[2][2]);
            const f32 invS = 0.5f / s;
            result.x = 0.5f * s;
            result.y = (m.m[0][1] + m.m[1][0]) * invS;
            result.z = (m.m[0][2] + m.m[2][0]) * invS;
            result.w = (m.m[1][2] - m.m[2][1]) * invS;
        }
        else if (m.m[1][1] > m.m[2][2])
        {
            const f32 s = Sqrt(1.0f + m.m[1][1] - m.m[0][0] - m.m[2][2]);
            const f32 invS = 0.5f / s;
            result.x = (m.m[1][0] + m.m[0][1]) * invS;
            result.y = 0.5f * s;
            result.z = (m.m[2][1] + m.m[1][2]) * invS;
            result.w = (m.m[2][0] - m.m[0][2]) * invS;
        }
        else
        {
            const f32 s = Sqrt(1.0f + m.m[2][2] - m.m[0][0] - m.m[1][1]);
            const f32 invS = 0.5f / s;
            result.x = (m.m[2][0] + m.m[0][2]) * invS;
            result.y = (m.m[2][1] + m.m[1][2]) * invS;
            result.z = 0.5f * s;
            result.w = (m.m[0][1] - m.m[1][0]) * invS;
        }
        return result;
    }

    // TRS decompose of a row-vector S*R*T matrix: translation (row 3), per-axis scale (basis row
    // lengths), rotation (normalized basis). False (identity outputs) when a scale axis is zero
    // (degenerate - rotation is unrecoverable). Ported from Sedulous Matrix.Decompose; like the
    // original, mirrored (negative-determinant) matrices land the sign on an arbitrary axis.
    [[nodiscard]] inline bool Decompose(const Float4x4& m, Float3& translation,
                                        Quaternion& rotation, Float3& scale) noexcept
    {
        translation = Float3{m.m[3][0], m.m[3][1], m.m[3][2]};

        scale.x = Sqrt(m.m[0][0] * m.m[0][0] + m.m[0][1] * m.m[0][1] + m.m[0][2] * m.m[0][2]);
        scale.y = Sqrt(m.m[1][0] * m.m[1][0] + m.m[1][1] * m.m[1][1] + m.m[1][2] * m.m[1][2]);
        scale.z = Sqrt(m.m[2][0] * m.m[2][0] + m.m[2][1] * m.m[2][1] + m.m[2][2] * m.m[2][2]);
        if (scale.x == 0.0f || scale.y == 0.0f || scale.z == 0.0f)
        {
            scale = Float3::One;
            rotation = Quaternion::Identity;
            return false;
        }
        // A mirrored basis (negative determinant) can't be a pure rotation: flip one axis.
        const f32 det = m.m[0][0] * (m.m[1][1] * m.m[2][2] - m.m[1][2] * m.m[2][1]) -
                        m.m[0][1] * (m.m[1][0] * m.m[2][2] - m.m[1][2] * m.m[2][0]) +
                        m.m[0][2] * (m.m[1][0] * m.m[2][1] - m.m[1][1] * m.m[2][0]);
        if (det < 0.0f)
        {
            scale.z = -scale.z;
        }

        Float4x4 r = Float4x4::Identity();
        for (i32 c = 0; c < 3; ++c)
        {
            r.m[0][c] = m.m[0][c] / scale.x;
            r.m[1][c] = m.m[1][c] / scale.y;
            r.m[2][c] = m.m[2][c] / scale.z;
        }
        rotation = QuaternionFromRotationMatrix(r);
        return true;
    }
}
