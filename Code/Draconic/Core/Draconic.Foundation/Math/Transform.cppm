// Draconic Foundation - :transform partition
// Transform: position / rotation / scale, composed as S * R * T into a Float4x4.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Debug/Assert.h"

export module draconic.foundation:transform;

import :base;
import :float3;
import :float4x4;
import :quaternion;

export namespace draconic::foundation
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
        // degenerate matrix). The editor's world-preserving reparent seam.
        [[nodiscard]] static Transform FromMatrix(const Float4x4& m) noexcept
        {
            Transform t;
            (void)Decompose(m, t.position, t.rotation, t.scale);
            return t;
        }

        // Component-wise interpolation: position/scale lerp, rotation slerp. (Sedulous BoneTransform.Lerp.)
        [[nodiscard]] static Transform Lerp(const Transform& a, const Transform& b, f32 t) noexcept
        {
            return Transform{
                draconic::foundation::Lerp(a.position, b.position, t),
                draconic::foundation::Slerp(a.rotation, b.rotation, t),
                draconic::foundation::Lerp(a.scale, b.scale, t),
            };
        }
    };

    // Identity transform (position 0, rotation identity, scale 1) - the default-constructed value.
    inline constexpr Transform IdentityTransform{};
}
