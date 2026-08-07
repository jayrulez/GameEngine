// Draconic Foundation - :float3x3 partition
// Float3x3: 3x3 row-major matrix (rotation / normal matrices) - multiply,
// Transpose/Determinant/Inverse, and FromMat4 (upper-left 3x3).
//
// Conventions: row-major storage m[row][col];
// row vectors (v' = v * M); composition left-to-right; XNA-style right-handed
// projections, NDC depth [0,1]; translation in the last row.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Debug/Assert.h"

export module draconic.foundation:float3x3;

import :base;
import :math;
import :float3;
import :float4x4;

export namespace draconic::foundation
{
    // =======================================================================
    // Float3x3 - 3x3, row-major, row-vector convention. Rotation / normal matrices.
    // =======================================================================
    struct Float3x3
    {
        f32 m[3][3];

        [[nodiscard]] constexpr f32 operator()(usize row, usize col) const noexcept
        {
            DRACONIC_ASSERT(row < 3 && col < 3);
            return m[row][col];
        }
        [[nodiscard]] constexpr f32& operator()(usize row, usize col) noexcept
        {
            DRACONIC_ASSERT(row < 3 && col < 3);
            return m[row][col];
        }

        [[nodiscard]] static constexpr Float3x3 Identity() noexcept
        {
            return Float3x3{{{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}}};
        }

        // Upper-left 3x3 of a Float4x4 (drops translation; the rotation/scale part).
        [[nodiscard]] static constexpr Float3x3 FromMat4(const Float4x4& mat) noexcept
        {
            return Float3x3{{{mat.m[0][0], mat.m[0][1], mat.m[0][2]},
                             {mat.m[1][0], mat.m[1][1], mat.m[1][2]},
                             {mat.m[2][0], mat.m[2][1], mat.m[2][2]}}};
        }
    };

    [[nodiscard]] constexpr Float3x3 operator*(const Float3x3& a, const Float3x3& b) noexcept
    {
        Float3x3 result{};
        for (usize row = 0; row < 3; ++row)
        {
            for (usize col = 0; col < 3; ++col)
            {
                f32 sum = 0.0f;
                for (usize k = 0; k < 3; ++k)
                {
                    sum += a.m[row][k] * b.m[k][col];
                }
                result.m[row][col] = sum;
            }
        }
        return result;
    }

    // Row-vector transform: v' = v * M.
    [[nodiscard]] constexpr Float3 operator*(Float3 v, const Float3x3& m) noexcept
    {
        return {v.x * m.m[0][0] + v.y * m.m[1][0] + v.z * m.m[2][0],
                v.x * m.m[0][1] + v.y * m.m[1][1] + v.z * m.m[2][1],
                v.x * m.m[0][2] + v.y * m.m[1][2] + v.z * m.m[2][2]};
    }

    [[nodiscard]] constexpr Float3x3 Transpose(const Float3x3& a) noexcept
    {
        Float3x3 result{};
        for (usize row = 0; row < 3; ++row)
        {
            for (usize col = 0; col < 3; ++col)
            {
                result.m[row][col] = a.m[col][row];
            }
        }
        return result;
    }

    [[nodiscard]] constexpr f32 Determinant(const Float3x3& m) noexcept
    {
        return m.m[0][0] * (m.m[1][1] * m.m[2][2] - m.m[1][2] * m.m[2][1]) -
               m.m[0][1] * (m.m[1][0] * m.m[2][2] - m.m[1][2] * m.m[2][0]) +
               m.m[0][2] * (m.m[1][0] * m.m[2][1] - m.m[1][1] * m.m[2][0]);
    }

    // 3x3 inverse (adjugate / determinant). Returns Identity if singular.
    [[nodiscard]] inline Float3x3 Inverse(const Float3x3& m) noexcept
    {
        const f32 det = Determinant(m);
        if (NearlyZero(det))
        {
            return Float3x3::Identity();
        }
        const f32 invDet = 1.0f / det;

        Float3x3 result{};
        result.m[0][0] = (m.m[1][1] * m.m[2][2] - m.m[1][2] * m.m[2][1]) * invDet;
        result.m[0][1] = (m.m[0][2] * m.m[2][1] - m.m[0][1] * m.m[2][2]) * invDet;
        result.m[0][2] = (m.m[0][1] * m.m[1][2] - m.m[0][2] * m.m[1][1]) * invDet;
        result.m[1][0] = (m.m[1][2] * m.m[2][0] - m.m[1][0] * m.m[2][2]) * invDet;
        result.m[1][1] = (m.m[0][0] * m.m[2][2] - m.m[0][2] * m.m[2][0]) * invDet;
        result.m[1][2] = (m.m[0][2] * m.m[1][0] - m.m[0][0] * m.m[1][2]) * invDet;
        result.m[2][0] = (m.m[1][0] * m.m[2][1] - m.m[1][1] * m.m[2][0]) * invDet;
        result.m[2][1] = (m.m[0][1] * m.m[2][0] - m.m[0][0] * m.m[2][1]) * invDet;
        result.m[2][2] = (m.m[0][0] * m.m[1][1] - m.m[0][1] * m.m[1][0]) * invDet;
        return result;
    }

    [[nodiscard]] inline bool NearlyEqual(const Float3x3& a, const Float3x3& b,
                                          f32 epsilon = kEpsilon) noexcept
    {
        for (usize row = 0; row < 3; ++row)
        {
            for (usize col = 0; col < 3; ++col)
            {
                if (!NearlyEqual(a.m[row][col], b.m[row][col], epsilon))
                {
                    return false;
                }
            }
        }
        return true;
    }
}
