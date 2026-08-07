// Draconic UI - :float_animation partition
//
// Animates a float value from a start to an end value via a setter delegate. Ported from
// Sedulous.UI/src/Animation/FloatAnimation.bf. Beef owned `delegate void(float) ~ delete _` ->
// Function<void(f32)> (value type, RAII).

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.ui:float_animation;

import draconic.foundation;
import :animation;

using namespace draconic::foundation;

export namespace draconic::ui
{
    class FloatAnimation : public Animation
    {
    public:
        FloatAnimation(f32 from, f32 to, f32 duration, Function<void(f32)> setter,
                       EasingFunction easing = nullptr)
            : Animation(duration, easing), m_from(from), m_to(to), m_setter(Move(setter))
        {
        }

        [[nodiscard]] f32 From() const noexcept { return m_from; }
        [[nodiscard]] f32 To() const noexcept { return m_to; }

    protected:
        void Apply(f32 t) override { m_setter(m_from + (m_to - m_from) * t); }

    private:
        f32 m_from;
        f32 m_to;
        Function<void(f32)> m_setter;
    };
}
