// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Core - :transform partition
// Transform: position / rotation / scale, composed as S * R * T into a Float4x4.

module;
#include "Core/Prelude.h"
#include "Core/Debug/Assert.h"

export module foundation.core:transform;

import :base;
import :float3;
import :float4x4;
import :quaternion;

export namespace foundation::core
{
    // =======================================================================
    // Transform - position / rotation / scale, composed as S * R * T.
    // =======================================================================
    struct Transform
    {
        Float3 position = Float3::Zero;
        Quaternion rotation = Quaternion::Identity;
        Float3 scale = Float3::One;

        [[nodiscard]] Float4x4 ToMatrix() const noexcept
        {
            Float4x4 result = Float4x4::Scale(scale) * RotationMatrix(rotation);
            result.m[3][0] = position.x;
            result.m[3][1] = position.y;
            result.m[3][2] = position.z;
            return result;
        }

        // ToMatrix's inverse: decompose a TRS matrix into a Transform (identity components on a
        // degenerate matrix). The editor reparents through it while keeping the world transform.
        [[nodiscard]] static Transform FromMatrix(const Float4x4& m) noexcept
        {
            Transform t;
            (void)Decompose(m, t.position, t.rotation, t.scale);
            return t;
        }

        // Component-wise interpolation: position/scale lerp, rotation slerp.
        [[nodiscard]] static Transform Lerp(const Transform& a, const Transform& b, f32 t) noexcept
        {
            return Transform{
                foundation::core::Lerp(a.position, b.position, t),
                foundation::core::Slerp(a.rotation, b.rotation, t),
                foundation::core::Lerp(a.scale, b.scale, t),
            };
        }
    };

    // Identity transform (position 0, rotation identity, scale 1), the default-constructed value.
    inline constexpr Transform IdentityTransform{};

    // The affine transform with scale removed (translation and rotation only). Use it where a
    // matrix places something whose dimensions are absolute in world units and must not be warped
    // by the placing entity's scale. A baked navmesh is one: its agent radius and cell size are in
    // world units, so both the bake frame and the runtime placement must be rigid. Otherwise a zone
    // sharing a scaled entity with its geometry would un-scale the geometry the bake sees and erode
    // the navmesh to nothing.
    [[nodiscard]] inline Float4x4 RigidPart(const Float4x4& m) noexcept
    {
        Transform t = Transform::FromMatrix(m);
        t.scale = Float3::One;
        return t.ToMatrix();
    }
}
