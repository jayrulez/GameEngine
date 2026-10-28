// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - :view_transform partition
//
// Post-layout transform applied during drawing and hit testing (does NOT affect layout).
// Applied in order: translate to origin, scale, rotate, translate back, then translate.
// Ported from Sedulous.UI/src/Core/ViewTransform.bf; Sedulous Vector2 -> core::Float2.

module;
#include "Core/Prelude.h"

export module foundation.ui:view_transform;

import foundation.core; // Float2

using namespace foundation::core;
namespace core = foundation::core;

export namespace foundation::ui
{
    // ENGINE_EXPORT_DATA for ViewTransform::Identity: a static data member read from
    // another library needs the annotation (shared-libraries.md P5/W1).
    struct ENGINE_EXPORT_DATA ViewTransform
    {
        /// Translation offset (pixels).
        core::Float2 Translation{0.0f, 0.0f};
        /// Rotation angle (radians).
        f32 Rotation = 0.0f;
        /// Scale factors. Default (1, 1).
        core::Float2 Scale{1.0f, 1.0f};
        /// Transform origin as a fraction of the view's size (0,0 = top-left, 0.5,0.5 = center).
        /// The pivot for rotation and scale.
        core::Float2 Origin{0.5f, 0.5f};

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
