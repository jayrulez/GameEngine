// Draconic GUI - :transformable partition
//
// Transformable: 2D position / rotation / scale with a lazily-recomputed combined
// Transform2D (+ its inverse). The math base of Node. Ported from eepp's
// Math::Transformable (include/eepp/math/transformable.hpp); rotation is RADIANS (eepp
// used degrees), and the setters are virtual so Node can hook invalidation.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.gui:transformable;

import draconic.foundation; // Float2
import :transform2d;

using namespace draconic::foundation;
namespace foundation = draconic::foundation;

export namespace draconic::gui
{
    class Transformable
    {
    public:
        Transformable() = default;
        virtual ~Transformable() = default;

        virtual void SetPosition(foundation::Float2 position)
        {
            m_position = position;
            MarkDirty();
        }
        virtual void SetRotation(f32 radians)
        {
            m_rotation = radians;
            MarkDirty();
        }
        virtual void SetScale(foundation::Float2 factors)
        {
            m_scale = factors;
            MarkDirty();
        }
        void SetScale(f32 x, f32 y) { SetScale(foundation::Float2{x, y}); }
        virtual void SetScaleOrigin(foundation::Float2 origin)
        {
            m_scaleOrigin = origin;
            MarkDirty();
        }
        virtual void SetRotationOrigin(foundation::Float2 origin)
        {
            m_rotationOrigin = origin;
            MarkDirty();
        }

        [[nodiscard]] foundation::Float2 GetPosition() const noexcept { return m_position; }
        [[nodiscard]] f32 GetRotation() const noexcept { return m_rotation; }
        [[nodiscard]] foundation::Float2 GetScale() const noexcept { return m_scale; }
        [[nodiscard]] foundation::Float2 GetScaleOrigin() const noexcept { return m_scaleOrigin; }
        [[nodiscard]] foundation::Float2 GetRotationOrigin() const noexcept { return m_rotationOrigin; }

        // Relative moves.
        void Move(foundation::Float2 offset) { SetPosition(m_position + offset); }
        void Rotate(f32 radians) { SetRotation(m_rotation + radians); }
        void Scale(foundation::Float2 factor) { SetScale(m_scale * factor); }

        [[nodiscard]] const Transform2D& GetTransform() const
        {
            if (m_transformDirty)
            {
                // Post-multiply order (eepp): T(pos) * [scale about origin] * [rotate about origin].
                Transform2D t;
                t.Translate(m_position);
                if (!(m_scale.x == 1.0f && m_scale.y == 1.0f))
                {
                    t.Translate(m_scaleOrigin);
                    t.Scale(m_scale);
                    t.Translate(-m_scaleOrigin);
                }
                if (m_rotation != 0.0f)
                {
                    t.Translate(m_rotationOrigin);
                    t.Rotate(m_rotation);
                    t.Translate(-m_rotationOrigin);
                }
                m_transform = t;
                m_transformDirty = false;
                m_inverseDirty = true;
            }
            return m_transform;
        }

        [[nodiscard]] const Transform2D& GetInverseTransform() const
        {
            if (m_inverseDirty || m_transformDirty)
            {
                m_inverse = GetTransform().GetInverse();
                m_inverseDirty = false;
            }
            return m_inverse;
        }

    protected:
        void MarkDirty() noexcept
        {
            m_transformDirty = true;
            m_inverseDirty = true;
        }

        foundation::Float2 m_position{0.0f, 0.0f};
        foundation::Float2 m_scale{1.0f, 1.0f};
        foundation::Float2 m_scaleOrigin{0.0f, 0.0f};
        foundation::Float2 m_rotationOrigin{0.0f, 0.0f};
        f32 m_rotation = 0.0f; // radians

        mutable Transform2D m_transform{};
        mutable Transform2D m_inverse{};
        mutable bool m_transformDirty = true;
        mutable bool m_inverseDirty = true;
    };
}
