// Draconic UI - :view_transform partition
//
// Post-layout transform applied during drawing and hit testing (does NOT affect layout).
// Applied in order: translate to origin, scale, rotate, translate back, then translate.
// Ported from Sedulous.UI/src/Core/ViewTransform.bf; Sedulous Vector2 -> foundation::Float2.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.ui:view_transform;

import draconic.foundation; // Float2

using namespace draconic::foundation;
namespace foundation = draconic::foundation;

export namespace draconic::ui
{
    struct ViewTransform
    {
        /// Translation offset (pixels).
        foundation::Float2 Translation{0.0f, 0.0f};
        /// Rotation angle (radians).
        f32 Rotation = 0.0f;
        /// Scale factors. Default (1, 1).
        foundation::Float2 Scale{1.0f, 1.0f};
        /// Transform origin as a fraction of the view's size (0,0 = top-left, 0.5,0.5 = center).
        /// The pivot for rotation and scale.
        foundation::Float2 Origin{0.5f, 0.5f};

        /// True if this transform has no visual effect.
        [[nodiscard]] constexpr bool IsIdentity() const noexcept
        {
            return Translation.x == 0.0f && Translation.y == 0.0f && Rotation == 0.0f &&
                   Scale.x == 1.0f && Scale.y == 1.0f;
        }

        /// Identity transform (no effect).
        static const ViewTransform Identity;
    };

    inline constexpr ViewTransform ViewTransform::Identity{};
}
