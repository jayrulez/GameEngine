// Draconic UI - :float2_animation partition
//
// Animates a Float2 value from a start to an end via a setter delegate. Ported from
// Sedulous.UI/src/Animation/Vector2Animation.bf (Sedulous Vector2 -> Draconic Float2, matching the
// port-wide UI math point). Owned delegate -> Function<void(Float2)>.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.ui:float2_animation;

import draconic.foundation;
import :animation;

using namespace draconic::foundation;

export namespace draconic::ui
{
    class Float2Animation : public Animation
    {
    public:
        Float2Animation(Float2 from, Float2 to, f32 duration, Function<void(Float2)> setter,
                        EasingFunction easing = nullptr)
            : Animation(duration, easing), m_from(from), m_to(to), m_setter(Move(setter))
        {
        }

        [[nodiscard]] Float2 From() const noexcept { return m_from; }
        [[nodiscard]] Float2 To() const noexcept { return m_to; }

    protected:
        void Apply(f32 t) override
        {
            m_setter(
                Float2{m_from.x + (m_to.x - m_from.x) * t, m_from.y + (m_to.y - m_from.y) * t});
        }

    private:
        Float2 m_from;
        Float2 m_to;
        Function<void(Float2)> m_setter;
    };
}
