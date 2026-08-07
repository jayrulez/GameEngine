// Draconic UI - :animation_manager partition
//
// Manages active animations. Owned by UIContext (a by-value member) and ticked each frame. Owns its
// animations (Beef `List<Animation> ~ DeleteContainerAndItems` -> Array<UniquePtr<Animation>>; RAII, no
// manual delete). Ported from Sedulous.UI/src/Animation/AnimationManager.bf. It touches no View methods
// (CancelForView only pointer-compares Target), so View is forward-declared and no impl unit is needed.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.ui:animation_manager;

import draconic.foundation;
import :animation;

using namespace draconic::foundation;

export namespace draconic::ui
{
    class View;

    class AnimationManager
    {
    public:
        AnimationManager() = default;

        AnimationManager(const AnimationManager&) = delete;
        AnimationManager& operator=(const AnimationManager&) = delete;

        /// Number of currently active animations.
        [[nodiscard]] usize ActiveCount() const noexcept
        {
            return m_animations.Size() + m_pending.Size();
        }

        /// Add an animation and start it. The AnimationManager takes ownership.
        void Add(UniquePtr<Animation> anim)
        {
            anim->Start();
            if (m_isUpdating)
            {
                m_pending.PushBack(Move(anim));
            }
            else
            {
                m_animations.PushBack(Move(anim));
            }
        }

        /// Tick all animations. Removes (and deletes) completed ones.
        void Update(f32 deltaTime)
        {
            m_isUpdating = true;

            for (i32 i = static_cast<i32>(m_animations.Size()) - 1; i >= 0; --i)
            {
                if (m_animations[static_cast<usize>(i)]->Update(deltaTime))
                {
                    m_animations.RemoveAtSwap(static_cast<usize>(i));
                }
            }

            m_isUpdating = false;

            // Merge pending animations added during Update.
            if (m_pending.Size() > 0)
            {
                for (UniquePtr<Animation>& anim : m_pending)
                {
                    m_animations.PushBack(Move(anim));
                }
                m_pending.Clear();
            }
        }

        /// Cancel and delete all animations.
        void CancelAll()
        {
            m_animations.Clear();
            m_pending.Clear();
        }

        /// Cancel and delete all animations targeting a specific view.
        void CancelForView(View* view)
        {
            for (i32 i = static_cast<i32>(m_animations.Size()) - 1; i >= 0; --i)
            {
                if (m_animations[static_cast<usize>(i)]->Target() == view)
                {
                    m_animations.RemoveAtSwap(static_cast<usize>(i));
                }
            }
            for (i32 i = static_cast<i32>(m_pending.Size()) - 1; i >= 0; --i)
            {
                if (m_pending[static_cast<usize>(i)]->Target() == view)
                {
                    m_pending.RemoveAtSwap(static_cast<usize>(i));
                }
            }
        }

    private:
        Array<UniquePtr<Animation>> m_animations;
        Array<UniquePtr<Animation>> m_pending;
        bool m_isUpdating = false;
    };
}
