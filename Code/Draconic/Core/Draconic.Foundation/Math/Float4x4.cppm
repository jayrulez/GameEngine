// Draconic Foundation - :float4x4 partition
// Float4x4: 4x4 row-major matrix - transforms, projections (Perspective/Ortho/
// LookAt RH), multiply, Transpose/Determinant/Inverse, point/direction xform.
//
// Conventions: row-major storage m[row][col];
// row vectors (v' = v * M); composition left-to-right; XNA-style right-handed
// projections, NDC depth [0,1]; translation in the last row.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Debug/Assert.h"

export module draconic.foundation:float4x4;

import :base;
import :math;
import :float2;
import :float3;
import :float4;

export namespace draconic::foundation
{
    // =======================================================================
    // Float4x4 - 4x4, row-major, row-vector convention.
    // =======================================================================
    struct Float4x4
    {
        f32 m[4][4];

        // Raw row-major float pointer (16 contiguous floats), e.g. for GPU upload.
        [[nodiscard]] const f32* Data() const noexcept { return &m[0][0]; }
        [[nodiscard]] f32* Data() noexcept { return &m[0][0]; }

        [[nodiscard]] constexpr f32 operator()(usize row, usize col) const noexcept
        {
            DRACONIC_ASSERT(row < 4 && col < 4);
            return m[row][col];
        }
        [[nodiscard]] constexpr f32& operator()(usize row, usize col) noexcept
        {
            DRACONIC_ASSERT(row < 4 && col < 4);
            return m[row][col];
        }

        [[nodiscard]] static constexpr Float4x4 Identity() noexcept
        {
            return Float4x4{{{1.0f, 0.0f, 0.0f, 0.0f},
                             {0.0f, 1.0f, 0.0f, 0.0f},
                             {0.0f, 0.0f, 1.0f, 0.0f},
                             {0.0f, 0.0f, 0.0f, 1.0f}}};
        }

        [[nodiscard]] static constexpr Float4x4 Translation(Float3 t) noexcept
        {
            return Float4x4{{{1.0f, 0.0f, 0.0f, 0.0f},
                             {0.0f, 1.0f, 0.0f, 0.0f},
                             {0.0f, 0.0f, 1.0f, 0.0f},
                             {t.x, t.y, t.z, 1.0f}}};
        }

        [[nodiscard]] static constexpr Float4x4 Scale(Float3 s) noexcept
        {
            return Float4x4{{{s.x, 0.0f, 0.0f, 0.0f},
                             {0.0f, s.y, 0.0f, 0.0f},
                             {0.0f, 0.0f, s.z, 0.0f},
                             {0.0f, 0.0f, 0.0f, 1.0f}}};
        }

        [[nodiscard]] static Float4x4 RotationX(f32 radians) noexcept
        {
            const f32 c = Cos(radians);
            const f32 s = Sin(radians);
            return Float4x4{{{1.0f, 0.0f, 0.0f, 0.0f},
                             {0.0f, c, s, 0.0f},
                             {0.0f, -s, c, 0.0f},
                             {0.0f, 0.0f, 0.0f, 1.0f}}};
        }

        [[nodiscard]] static Float4x4 RotationY(f32 radians) noexcept
        {
            const f32 c = Cos(radians);
            const f32 s = Sin(radians);
            return Float4x4{{{c, 0.0f, -s, 0.0f},
                             {0.0f, 1.0f, 0.0f, 0.0f},
                             {s, 0.0f, c, 0.0f},
                             {0.0f, 0.0f, 0.0f, 1.0f}}};
        }

        [[nodiscard]] static Float4x4 RotationZ(f32 radians) noexcept
        {
            const f32 c = Cos(radians);
            const f32 s = Sin(radians);
            return Float4x4{{{c, s, 0.0f, 0.0f},
                             {-s, c, 0.0f, 0.0f},
                             {0.0f, 0.0f, 1.0f, 0.0f},
                             {0.0f, 0.0f, 0.0f, 1.0f}}};
        }

        // Right-handed perspective, NDC z in [0, 1] (XNA / D3D style).
        [[nodiscard]] static Float4x4 PerspectiveFovRH(f32 fovYRadians, f32 aspect, f32 zNear,
                                                       f32 zFar) noexcept
        {
            const f32 yScale = 1.0f / Tan(fovYRadians * 0.5f);
            const f32 xScale = yScale / aspect;
            const f32 zRange = zFar / (zNear - zFar);
            return Float4x4{{{xScale, 0.0f, 0.0f, 0.0f},
                             {0.0f, yScale, 0.0f, 0.0f},
                             {0.0f, 0.0f, zRange, -1.0f},
                             {0.0f, 0.0f, zNear * zRange, 0.0f}}};
        }

        [[nodiscard]] static Float4x4 OrthographicRH(f32 width, f32 height, f32 zNear,
                                                     f32 zFar) noexcept
        {
            const f32 zRange = 1.0f / (zNear - zFar);
            return Float4x4{{{2.0f / width, 0.0f, 0.0f, 0.0f},
                             {0.0f, 2.0f / height, 0.0f, 0.0f},
                             {0.0f, 0.0f, zRange, 0.0f},
                             {0.0f, 0.0f, zNear * zRange, 1.0f}}};
        }

        [[nodiscard]] static Float4x4 LookAtRH(Float3 eye, Float3 target, Float3 up) noexcept
        {
            const Float3 zAxis = Normalized(eye - target); // camera looks down -z
            const Float3 xAxis = Normalized(Cross(up, zAxis));
            const Float3 yAxis = Cross(zAxis, xAxis);
            return Float4x4{{{xAxis.x, yAxis.x, zAxis.x, 0.0f},
                             {xAxis.y, yAxis.y, zAxis.y, 0.0f},
                             {xAxis.z, yAxis.z, zAxis.z, 0.0f},
                             {-Dot(xAxis, eye), -Dot(yAxis, eye), -Dot(zAxis, eye), 1.0f}}};
        }
    };

    [[nodiscard]] constexpr Float4x4 operator*(const Float4x4& a, const Float4x4& b) noexcept
    {
        Float4x4 result{};
        for (usize row = 0; row < 4; ++row)
        {
            for (usize col = 0; col < 4; ++col)
            {
                f32 sum = 0.0f;
                for (usize k = 0; k < 4; ++k)
                {
                    sum += a.m[row][k] * b.m[k][col];
                }
                result.m[row][col] = sum;
            }
        }
        return result;
    }

    // Row-vector transform: v' = v * M.
    [[nodiscard]] constexpr Float4 operator*(Float4 v, const Float4x4& m) noexcept
    {
        return {v.x * m.m[0][0] + v.y * m.m[1][0] + v.z * m.m[2][0] + v.w * m.m[3][0],
                v.x * m.m[0][1] + v.y * m.m[1][1] + v.z * m.m[2][1] + v.w * m.m[3][1],
                v.x * m.m[0][2] + v.y * m.m[1][2] + v.z * m.m[2][2] + v.w * m.m[3][2],
                v.x * m.m[0][3] + v.y * m.m[1][3] + v.z * m.m[2][3] + v.w * m.m[3][3]};
    }

    [[nodiscard]] constexpr Float4x4 Transpose(const Float4x4& a) noexcept
    {
        Float4x4 result{};
        for (usize row = 0; row < 4; ++row)
        {
            for (usize col = 0; col < 4; ++col)
            {
                result.m[row][col] = a.m[col][row];
            }
        }
        return result;
    }

    // Transforms a position (implicit w = 1, translation applied).
    [[nodiscard]] constexpr Float3 TransformPoint(Float3 p, const Float4x4& m) noexcept
    {
        return {p.x * m.m[0][0] + p.y * m.m[1][0] + p.z * m.m[2][0] + m.m[3][0],
                p.x * m.m[0][1] + p.y * m.m[1][1] + p.z * m.m[2][1] + m.m[3][1],
                p.x * m.m[0][2] + p.y * m.m[1][2] + p.z * m.m[2][2] + m.m[3][2]};
    }

    // Transforms a direction (implicit w = 0, translation ignored).
    [[nodiscard]] constexpr Float3 TransformDirection(Float3 d, const Float4x4& m) noexcept
    {
        return {d.x * m.m[0][0] + d.y * m.m[1][0] + d.z * m.m[2][0],
                d.x * m.m[0][1] + d.y * m.m[1][1] + d.z * m.m[2][1],
                d.x * m.m[0][2] + d.y * m.m[1][2] + d.z * m.m[2][2]};
    }

    // Transforms a 2D position (implicit z = 0, w = 1, translation applied;
    // result projected back to 2D). For 2D affine transforms stored in a Float4x4.
    [[nodiscard]] constexpr Float2 TransformPoint2D(Float2 p, const Float4x4& m) noexcept
    {
        return {p.x * m.m[0][0] + p.y * m.m[1][0] + m.m[3][0],
                p.x * m.m[0][1] + p.y * m.m[1][1] + m.m[3][1]};
    }

    // Exact element-wise equality (e.g. for an identity fast-path).
    [[nodiscard]] constexpr bool operator==(const Float4x4& a, const Float4x4& b) noexcept
    {
        for (usize row = 0; row < 4; ++row)
            for (usize col = 0; col < 4; ++col)
                if (a.m[row][col] != b.m[row][col])
                {
                    return false;
                }
        return true;
    }

    [[nodiscard]] inline bool NearlyEqual(const Float4x4& a, const Float4x4& b,
                                          f32 epsilon = kEpsilon) noexcept
    {
        for (usize row = 0; row < 4; ++row)
        {
            for (usize col = 0; col < 4; ++col)
            {
                if (!NearlyEqual(a.m[row][col], b.m[row][col], epsilon))
                {
                    return false;
                }
            }
        }
        return true;
    }

    [[nodiscard]] inline f32 Determinant(const Float4x4& mat) noexcept
    {
        const f32* m = &mat.m[0][0];
        const f32 s0 = m[0] * m[5] - m[1] * m[4];
        const f32 s1 = m[0] * m[6] - m[2] * m[4];
        const f32 s2 = m[0] * m[7] - m[3] * m[4];
        const f32 s3 = m[1] * m[6] - m[2] * m[5];
        const f32 s4 = m[1] * m[7] - m[3] * m[5];
        const f32 s5 = m[2] * m[7] - m[3] * m[6];
        const f32 c5 = m[10] * m[15] - m[11] * m[14];
        const f32 c4 = m[9] * m[15] - m[11] * m[13];
        const f32 c3 = m[9] * m[14] - m[10] * m[13];
        const f32 c2 = m[8] * m[15] - m[11] * m[12];
        const f32 c1 = m[8] * m[14] - m[10] * m[12];
        const f32 c0 = m[8] * m[13] - m[9] * m[12];
        return s0 * c5 - s1 * c4 + s2 * c3 + s3 * c2 - s4 * c1 + s5 * c0;
    }

    // Full 4x4 inverse (adjugate / determinant). Returns Identity for a
    // singular matrix.
    [[nodiscard]] inline Float4x4 Inverse(const Float4x4& mat) noexcept
    {
        const f32* m = &mat.m[0][0];
        f32 inv[16];

        inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] +
                 m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
        inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] -
                 m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
        inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] +
                 m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
        inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] -
                  m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
        inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] -
                 m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
        inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] +
                 m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
        inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] -
                 m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
        inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] +
                  m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
        inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] +
                 m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
        inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] -
                 m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
        inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] +
                  m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
        inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] -
                  m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
        inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] -
                 m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
        inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] +
                 m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
        inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] -
                  m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
        inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] +
                  m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];

        f32 det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
        if (NearlyZero(det))
        {
            return Float4x4::Identity();
        }

        const f32 invDet = 1.0f / det;
        Float4x4 result{};
        f32* out = &result.m[0][0];
        for (usize i = 0; i < 16; ++i)
        {
            out[i] = inv[i] * invDet;
        }
        return result;
    }
}
