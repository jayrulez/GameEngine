// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - :easing partition
//
// Convenience re-exports of foundation.core's easing functions (:easings) with short, UI-friendly names.
// Ported from Sedulous.UI/src/Animation/Easing.bf (Beef `static class` of readonly EasingFunction ->
// a struct of static constexpr function-pointer members). EasingFunction = f32(*)(f32).

module;
#include "Core/Prelude.h"

export module foundation.ui:easing;

import foundation.core;

using namespace foundation::core;

export namespace foundation::ui
{
    /// Short UI-friendly names for the core easing functions.
    // ENGINE_EXPORT_DATA: these are static DATA members (function pointers). Taking one
    // by address from another library needs the annotation - a .def entry is not enough,
    // since only a dllimport declaration changes the use site. shared-libraries.md P5/W1.
    struct ENGINE_EXPORT_DATA Easing
    {
        static constexpr EasingFunction Linear = &EaseInLinear;

        // Quadratic
        static constexpr EasingFunction EaseIn = &EaseInQuadratic;
        static constexpr EasingFunction EaseOut = &EaseOutQuadratic;
        static constexpr EasingFunction EaseInOut = &EaseInOutQuadratic;

        // Cubic (default for smooth UI animations)
        static constexpr EasingFunction EaseInCubic = &foundation::core::EaseInCubic;
        static constexpr EasingFunction EaseOutCubic = &foundation::core::EaseOutCubic;
        static constexpr EasingFunction EaseInOutCubic = &foundation::core::EaseInOutCubic;

        // Quartic
        static constexpr EasingFunction EaseInQuartic = &foundation::core::EaseInQuartic;
        static constexpr EasingFunction EaseOutQuartic = &foundation::core::EaseOutQuartic;
        static constexpr EasingFunction EaseInOutQuartic = &foundation::core::EaseInOutQuartic;

        // Quintic
        static constexpr EasingFunction EaseInQuintic = &foundation::core::EaseInQuintic;
        static constexpr EasingFunction EaseOutQuintic = &foundation::core::EaseOutQuintic;
        static constexpr EasingFunction EaseInOutQuintic = &foundation::core::EaseInOutQuintic;

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
