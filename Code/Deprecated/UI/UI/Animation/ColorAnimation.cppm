// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - :color_animation partition
//
// Animates a Color value from a start to an end color via a setter delegate. Ported from
// Sedulous.UI/src/Animation/ColorAnimation.bf. Beef `Color.Lerp` -> core::Lerp(Color,Color,f32) (free
// function); owned delegate -> Function<void(Color)>.

module;
#include "Core/Prelude.h"

export module foundation.ui:color_animation;

import foundation.core;
import :animation;

using namespace foundation::core;

export namespace foundation::ui
{
    class ColorAnimation : public Animation
    {
    public:
        ColorAnimation(Color from, Color to, f32 duration, Function<void(Color)> setter,
                       EasingFunction easing = nullptr)
            : Animation(duration, easing), m_from(from), m_to(to), m_setter(Move(setter))
        {
        }

        [[nodiscard]] Color From() const noexcept { return m_from; }
        [[nodiscard]] Color To() const noexcept { return m_to; }

    protected:
        void Apply(f32 t) override { m_setter(Lerp(m_from, m_to, t)); }

    private:
        Color m_from;
        Color m_to;
        Function<void(Color)> m_setter;
    };
}
