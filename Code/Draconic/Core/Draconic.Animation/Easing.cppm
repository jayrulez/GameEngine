/// Draconic::Animation - the `:easing` partition.
///
/// EasingType: a serializable enum mapping 1:1 to the foundation easing functions (Draconic.Foundation :easings).
/// Ported faithfully from Sedulous.Animation.EasingType. The functions themselves live in foundation math;
/// this is the animation-facing enum + lookup.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.animation:easing;

import draconic.foundation;

using namespace draconic::foundation;

export namespace draconic::animation
{

    // Serializable easing type (1:1 with the foundation Easings family). Order matters - it's the serialized
    // value and several tools index by it.
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

    // Maps EasingType -> the corresponding foundation easing function (never null).
    [[nodiscard]] inline foundation::EasingFunction ToFunction(EasingType type) noexcept
    {
        switch (type)
        {
        case EasingType::Linear:
            return foundation::EaseInLinear;
        case EasingType::EaseInQuadratic:
            return foundation::EaseInQuadratic;
        case EasingType::EaseOutQuadratic:
            return foundation::EaseOutQuadratic;
        case EasingType::EaseInOutQuadratic:
            return foundation::EaseInOutQuadratic;
        case EasingType::EaseInCubic:
            return foundation::EaseInCubic;
        case EasingType::EaseOutCubic:
            return foundation::EaseOutCubic;
        case EasingType::EaseInOutCubic:
            return foundation::EaseInOutCubic;
        case EasingType::EaseInQuartic:
            return foundation::EaseInQuartic;
        case EasingType::EaseOutQuartic:
            return foundation::EaseOutQuartic;
        case EasingType::EaseInOutQuartic:
            return foundation::EaseInOutQuartic;
        case EasingType::EaseInQuintic:
            return foundation::EaseInQuintic;
        case EasingType::EaseOutQuintic:
            return foundation::EaseOutQuintic;
        case EasingType::EaseInOutQuintic:
            return foundation::EaseInOutQuintic;
        case EasingType::EaseInSin:
            return foundation::EaseInSin;
        case EasingType::EaseOutSin:
            return foundation::EaseOutSin;
        case EasingType::EaseInOutSin:
            return foundation::EaseInOutSin;
        case EasingType::EaseInExponential:
            return foundation::EaseInExponential;
        case EasingType::EaseOutExponential:
            return foundation::EaseOutExponential;
        case EasingType::EaseInOutExponential:
            return foundation::EaseInOutExponential;
        case EasingType::EaseInCircular:
            return foundation::EaseInCircular;
        case EasingType::EaseOutCircular:
            return foundation::EaseOutCircular;
        case EasingType::EaseInOutCircular:
            return foundation::EaseInOutCircular;
        case EasingType::EaseInBack:
            return foundation::EaseInBack;
        case EasingType::EaseOutBack:
            return foundation::EaseOutBack;
        case EasingType::EaseInOutBack:
            return foundation::EaseInOutBack;
        case EasingType::EaseInElastic:
            return foundation::EaseInElastic;
        case EasingType::EaseOutElastic:
            return foundation::EaseOutElastic;
        case EasingType::EaseInOutElastic:
            return foundation::EaseInOutElastic;
        case EasingType::EaseInBounce:
            return foundation::EaseInBounce;
        case EasingType::EaseOutBounce:
            return foundation::EaseOutBounce;
        case EasingType::EaseInOutBounce:
            return foundation::EaseInOutBounce;
        default:
            return foundation::EaseInLinear;
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

} // namespace draconic::animation
