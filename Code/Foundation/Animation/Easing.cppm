// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Foundation::Animation - the `:easing` partition.
///
/// EasingType: a serializable enum mapping 1:1 to the core easing functions (foundation.core
/// :easings). The functions themselves live in core math; this is the animation-facing enum and
/// lookup.

module;
#include "Core/Prelude.h"

export module foundation.animation:easing;

import foundation.core;

using namespace foundation::core;
namespace core = foundation::core;

export namespace foundation::animation
{

    // Serializable easing type (1:1 with the core Easings family). Order matters: it is the
    // serialized value, and several tools index by it.
    enum class EasingType : i32
    {
        Linear = 0,
        EaseInQuadratic,
        EaseOutQuadratic,
        EaseInOutQuadratic,
        EaseInCubic,
        EaseOutCubic,
        EaseInOutCubic,
        EaseInQuartic,
        EaseOutQuartic,
        EaseInOutQuartic,
        EaseInQuintic,
        EaseOutQuintic,
        EaseInOutQuintic,
        EaseInSin,
        EaseOutSin,
        EaseInOutSin,
        EaseInExponential,
        EaseOutExponential,
        EaseInOutExponential,
        EaseInCircular,
        EaseOutCircular,
        EaseInOutCircular,
        EaseInBack,
        EaseOutBack,
        EaseInOutBack,
        EaseInElastic,
        EaseOutElastic,
        EaseInOutElastic,
        EaseInBounce,
        EaseOutBounce,
        EaseInOutBounce,
        Count
    };

    // Maps an EasingType to its core easing function (never null).
    [[nodiscard]] inline core::EasingFunction ToFunction(EasingType type) noexcept
    {
        switch (type)
        {
        case EasingType::Linear:
            return core::EaseInLinear;
        case EasingType::EaseInQuadratic:
            return core::EaseInQuadratic;
        case EasingType::EaseOutQuadratic:
            return core::EaseOutQuadratic;
        case EasingType::EaseInOutQuadratic:
            return core::EaseInOutQuadratic;
        case EasingType::EaseInCubic:
            return core::EaseInCubic;
        case EasingType::EaseOutCubic:
            return core::EaseOutCubic;
        case EasingType::EaseInOutCubic:
            return core::EaseInOutCubic;
        case EasingType::EaseInQuartic:
            return core::EaseInQuartic;
        case EasingType::EaseOutQuartic:
            return core::EaseOutQuartic;
        case EasingType::EaseInOutQuartic:
            return core::EaseInOutQuartic;
        case EasingType::EaseInQuintic:
            return core::EaseInQuintic;
        case EasingType::EaseOutQuintic:
            return core::EaseOutQuintic;
        case EasingType::EaseInOutQuintic:
            return core::EaseInOutQuintic;
        case EasingType::EaseInSin:
            return core::EaseInSin;
        case EasingType::EaseOutSin:
            return core::EaseOutSin;
        case EasingType::EaseInOutSin:
            return core::EaseInOutSin;
        case EasingType::EaseInExponential:
            return core::EaseInExponential;
        case EasingType::EaseOutExponential:
            return core::EaseOutExponential;
        case EasingType::EaseInOutExponential:
            return core::EaseInOutExponential;
        case EasingType::EaseInCircular:
            return core::EaseInCircular;
        case EasingType::EaseOutCircular:
            return core::EaseOutCircular;
        case EasingType::EaseInOutCircular:
            return core::EaseInOutCircular;
        case EasingType::EaseInBack:
            return core::EaseInBack;
        case EasingType::EaseOutBack:
            return core::EaseOutBack;
        case EasingType::EaseInOutBack:
            return core::EaseInOutBack;
        case EasingType::EaseInElastic:
            return core::EaseInElastic;
        case EasingType::EaseOutElastic:
            return core::EaseOutElastic;
        case EasingType::EaseInOutElastic:
            return core::EaseInOutElastic;
        case EasingType::EaseInBounce:
            return core::EaseInBounce;
        case EasingType::EaseOutBounce:
            return core::EaseOutBounce;
        case EasingType::EaseInOutBounce:
            return core::EaseInOutBounce;
        default:
            return core::EaseInLinear;
        }
    }

    // Applies an easing function to an interpolation factor t in [0,1].
    [[nodiscard]] inline f32 ApplyEasing(EasingType type, f32 t) noexcept
    {
        if (type == EasingType::Linear)
        {
            return t;
        }
        return ToFunction(type)(t);
    }

} // namespace foundation::animation
