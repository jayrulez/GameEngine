// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - :transform2d partition
//
// Transform2D: a 2D affine transform, what a sheet's `transform` composes and a view is drawn and
// hit-tested through. Six floats in column-vector affine form:
//
//     | a  c  tx |          x' = a*x + c*y + tx
//     | b  d  ty |          y' = b*x + d*y + ty
//
// ToMatrix() lays them into a Float4x4 in VG's convention (row vectors, translation in row 3), so
// a view's transform feeds VGContext::SetTransform losslessly. The GUI's Transform2D has the
// same form.

module;
#include "Core/Prelude.h"

export module foundation.ui:transform2d;

import foundation.core; // Float2, Float4x4, Cos, Sin

using namespace foundation::core;
namespace core = foundation::core;

export namespace foundation::ui
{
    struct Transform2D
    {
        f32 a = 1.0f, b = 0.0f;
        f32 c = 0.0f, d = 1.0f;
        f32 tx = 0.0f, ty = 0.0f;

        constexpr Transform2D() noexcept = default;
        constexpr Transform2D(f32 inA, f32 inB, f32 inC, f32 inD, f32 inTx, f32 inTy) noexcept
            : a(inA), b(inB), c(inC), d(inD), tx(inTx), ty(inTy)
        {
        }

        [[nodiscard]] constexpr bool IsIdentity() const noexcept
        {
            return a == 1.0f && b == 0.0f && c == 0.0f && d == 1.0f && tx == 0.0f && ty == 0.0f;
        }
        [[nodiscard]] constexpr bool operator==(const Transform2D&) const noexcept = default;

        [[nodiscard]] constexpr core::Float2 Apply(core::Float2 p) const noexcept
        {
            return core::Float2{a * p.x + c * p.y + tx, b * p.x + d * p.y + ty};
        }

        /// `l` then `r` read right to left: Multiply(l, r).Apply(p) == l.Apply(r.Apply(p)).
        [[nodiscard]] static constexpr Transform2D Multiply(const Transform2D& l, const Transform2D& r) noexcept
        {
            return Transform2D{l.a * r.a + l.c * r.b, l.b * r.a + l.d * r.b,
                               l.a * r.c + l.c * r.d, l.b * r.c + l.d * r.d,
                               l.a * r.tx + l.c * r.ty + l.tx, l.b * r.tx + l.d * r.ty + l.ty};
        }
        /// this = this * r: what `r` does happens first, inside this.
        constexpr Transform2D& Combine(const Transform2D& r) noexcept
        {
            *this = Multiply(*this, r);
            return *this;
        }
        constexpr Transform2D& Translate(f32 x, f32 y) noexcept { return Combine(Transform2D{1, 0, 0, 1, x, y}); }
        constexpr Transform2D& Scale(f32 sx, f32 sy) noexcept { return Combine(Transform2D{sx, 0, 0, sy, 0, 0}); }
        Transform2D& Rotate(f32 radians) noexcept
        {
            const f32 cs = core::Cos(radians), sn = core::Sin(radians);
            return Combine(Transform2D{cs, sn, -sn, cs, 0.0f, 0.0f});
        }
        /// CSS's skew(ax, ay): x' = x + tan(ax) y, y' = tan(ay) x + y.
        Transform2D& Skew(f32 radiansX, f32 radiansY) noexcept
        {
            return Combine(Transform2D{1.0f, core::Tan(radiansY), core::Tan(radiansX), 1.0f, 0.0f, 0.0f});
        }

        /// The inverse; identity for a transform that flattens (a scale of 0).
        [[nodiscard]] Transform2D Inverse() const noexcept
        {
            const f32 det = a * d - c * b;
            if (core::Abs(det) < 1e-12f)
            {
                return Transform2D{};
            }
            const f32 inv = 1.0f / det;
            const f32 ia = d * inv, ic = -c * inv;
            const f32 ib = -b * inv, id = a * inv;
            return Transform2D{ia, ib, ic, id, -(ia * tx + ic * ty), -(ib * tx + id * ty)};
        }

        /// The uniform scale, when the transform is only a scale and a translation (no rotation,
        /// skew or uneven scale).
        [[nodiscard]] constexpr bool UniformScale(f32& out) const noexcept
        {
            if (b == 0.0f && c == 0.0f && a == d && a > 0.0f)
            {
                out = a;
                return true;
            }
            return false;
        }

        /// In VG's convention: row vectors, translation in row 3.
        [[nodiscard]] constexpr core::Float4x4 ToMatrix() const noexcept
        {
            core::Float4x4 m = core::Float4x4::Identity();
            m.m[0][0] = a;
            m.m[0][1] = b;
            m.m[1][0] = c;
            m.m[1][1] = d;
            m.m[3][0] = tx;
            m.m[3][1] = ty;
            return m;
        }
    };
}
