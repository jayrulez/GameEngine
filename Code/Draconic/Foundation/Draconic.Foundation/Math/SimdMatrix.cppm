// Draconic Foundation - :simd_matrix partition
//
// Matrix4: aligned (16-byte) SIMD 4x4 matrix, backed by four simd::f32x4 rows. Same convention as the
// packed Float4x4 (row-major storage, row vectors: v' = v * M, left-to-right composition). Reach for
// this in hot CPU paths (transform-hierarchy multiply, skinning palettes); store as Float4x4 and Load
// to compute. Construction helpers (perspective/lookat/rotation) stay on the packed Float4x4 - build
// there and Load a Matrix4 when you need to multiply many.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Math/SimdConfig.h"

export module draconic.foundation:simd_matrix;

import :base;
import :simd;
import :simd_vector;
import :float4x4;

namespace draconic::foundation::detail
{
    // Row-vector * matrix: v.x*row0 + v.y*row1 + v.z*row2 + v.w*row3.
    [[nodiscard]] inline simd::f32x4 TransformRow(simd::f32x4 v, const simd::f32x4 rows[4]) noexcept
    {
        simd::f32x4 res = simd::Mul(simd::SplatX(v), rows[0]);
        res = simd::Add(res, simd::Mul(simd::SplatY(v), rows[1]));
        res = simd::Add(res, simd::Mul(simd::SplatZ(v), rows[2]));
        res = simd::Add(res, simd::Mul(simd::SplatW(v), rows[3]));
        return res;
    }
}

export namespace draconic::foundation
{
    struct alignas(16) Matrix4
    {
        simd::f32x4 row[4];

        // Default = identity.
        Matrix4() noexcept
        {
            row[0] = simd::Set(1.0f, 0.0f, 0.0f, 0.0f);
            row[1] = simd::Set(0.0f, 1.0f, 0.0f, 0.0f);
            row[2] = simd::Set(0.0f, 0.0f, 1.0f, 0.0f);
            row[3] = simd::Set(0.0f, 0.0f, 0.0f, 1.0f);
        }
        Matrix4(simd::f32x4 r0, simd::f32x4 r1, simd::f32x4 r2, simd::f32x4 r3) noexcept
        {
            row[0] = r0;
            row[1] = r1;
            row[2] = r2;
            row[3] = r3;
        }
        explicit Matrix4(const Float4x4& m) noexcept
        {
            row[0] = simd::Set(m.m[0][0], m.m[0][1], m.m[0][2], m.m[0][3]);
            row[1] = simd::Set(m.m[1][0], m.m[1][1], m.m[1][2], m.m[1][3]);
            row[2] = simd::Set(m.m[2][0], m.m[2][1], m.m[2][2], m.m[2][3]);
            row[3] = simd::Set(m.m[3][0], m.m[3][1], m.m[3][2], m.m[3][3]);
        }

        [[nodiscard]] Float4x4 ToFloat4x4() const noexcept
        {
            Float4x4 out{};
            for (int i = 0; i < 4; ++i)
            {
                f32 b[4];
                simd::Store(row[i], b);
                out.m[i][0] = b[0];
                out.m[i][1] = b[1];
                out.m[i][2] = b[2];
                out.m[i][3] = b[3];
            }
            return out;
        }

        [[nodiscard]] static Matrix4 Identity() noexcept { return Matrix4(); }
    };

    // Matrix * matrix (row-major, row-vector): result row i = (a row i) * b.
    [[nodiscard]] inline Matrix4 operator*(const Matrix4& a, const Matrix4& b) noexcept
    {
        return Matrix4(detail::TransformRow(a.row[0], b.row), detail::TransformRow(a.row[1], b.row),
                       detail::TransformRow(a.row[2], b.row),
                       detail::TransformRow(a.row[3], b.row));
    }

    // Row-vector transform: v' = v * M.
    [[nodiscard]] inline Vector4 operator*(Vector4 v, const Matrix4& m) noexcept
    {
        return Vector4(detail::TransformRow(v.r, m.row));
    }

    // Position (implicit w = 1, translation applied); result is xyz.
    [[nodiscard]] inline Vector3 TransformPoint(Vector3 p, const Matrix4& m) noexcept
    {
        simd::f32x4 res = simd::Mul(simd::SplatX(p.r), m.row[0]);
        res = simd::Add(res, simd::Mul(simd::SplatY(p.r), m.row[1]));
        res = simd::Add(res, simd::Mul(simd::SplatZ(p.r), m.row[2]));
        res = simd::Add(res, m.row[3]); // implicit w = 1
        return Vector3(res);
    }

    // Direction (implicit w = 0, translation ignored).
    [[nodiscard]] inline Vector3 TransformDirection(Vector3 d, const Matrix4& m) noexcept
    {
        simd::f32x4 res = simd::Mul(simd::SplatX(d.r), m.row[0]);
        res = simd::Add(res, simd::Mul(simd::SplatY(d.r), m.row[1]));
        res = simd::Add(res, simd::Mul(simd::SplatZ(d.r), m.row[2]));
        return Vector3(res);
    }

    // Transpose (infrequent; delegates to the tested packed path).
    [[nodiscard]] inline Matrix4 Transpose(const Matrix4& m) noexcept
    {
        return Matrix4(Transpose(m.ToFloat4x4()));
    }
}
