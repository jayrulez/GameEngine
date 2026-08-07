// Draconic UI - :easing partition
//
// Convenience re-exports of Draconic.Foundation's easing functions (:easings) with short, UI-friendly names.
// Ported from Sedulous.UI/src/Animation/Easing.bf (Beef `static class` of readonly EasingFunction ->
// a struct of static constexpr function-pointer members). EasingFunction = f32(*)(f32).

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.ui:easing;

import draconic.foundation;

using namespace draconic::foundation;

export namespace draconic::ui
{
    /// Short UI-friendly names for the core easing functions.
    struct Easing
    {
        static constexpr EasingFunction Linear = &EaseInLinear;

        // Quadratic
        static constexpr EasingFunction EaseIn = &EaseInQuadratic;
        static constexpr EasingFunction EaseOut = &EaseOutQuadratic;
        static constexpr EasingFunction EaseInOut = &EaseInOutQuadratic;

        // Cubic (default for smooth UI animations)
        static constexpr EasingFunction EaseInCubic = &draconic::foundation::EaseInCubic;
        static constexpr EasingFunction EaseOutCubic = &draconic::foundation::EaseOutCubic;
        static constexpr EasingFunction EaseInOutCubic = &draconic::foundation::EaseInOutCubic;

        // Quartic
        static constexpr EasingFunction EaseInQuartic = &draconic::foundation::EaseInQuartic;
        static constexpr EasingFunction EaseOutQuartic = &draconic::foundation::EaseOutQuartic;
        static constexpr EasingFunction EaseInOutQuartic = &draconic::foundation::EaseInOutQuartic;

        // Quintic
        static constexpr EasingFunction EaseInQuintic = &draconic::foundation::EaseInQuintic;
        static constexpr EasingFunction EaseOutQuintic = &draconic::foundation::EaseOutQuintic;
        static constexpr EasingFunction EaseInOutQuintic = &draconic::foundation::EaseInOutQuintic;

        // Bounce
        static constexpr EasingFunction BounceIn = &EaseInBounce;
        static constexpr EasingFunction BounceOut = &EaseOutBounce;
        static constexpr EasingFunction BounceInOut = &EaseInOutBounce;

        // Elastic
        static constexpr EasingFunction ElasticIn = &EaseInElastic;
        static constexpr EasingFunction ElasticOut = &EaseOutElastic;
        static constexpr EasingFunction ElasticInOut = &EaseInOutElastic;

        // Back (overshoot)
        static constexpr EasingFunction BackIn = &EaseInBack;
        static constexpr EasingFunction BackOut = &EaseOutBack;
        static constexpr EasingFunction BackInOut = &EaseInOutBack;

        // Exponential
        static constexpr EasingFunction ExpoIn = &EaseInExponential;
        static constexpr EasingFunction ExpoOut = &EaseOutExponential;
        static constexpr EasingFunction ExpoInOut = &EaseInOutExponential;

        // Sinusoidal
        static constexpr EasingFunction SineIn = &EaseInSin;
        static constexpr EasingFunction SineOut = &EaseOutSin;
        static constexpr EasingFunction SineInOut = &EaseInOutSin;

        // Circular
        static constexpr EasingFunction CircIn = &EaseInCircular;
        static constexpr EasingFunction CircOut = &EaseOutCircular;
        static constexpr EasingFunction CircInOut = &EaseInOutCircular;
    };
}
