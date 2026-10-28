// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - :storyboard partition
//
// Groups multiple animations to play sequentially or in parallel. A Storyboard is itself an Animation, so
// it can be nested. Ported from Sedulous.UI/src/Animation/Storyboard.bf. Beef `List<Animation> ~ Delete
// ContainerAndItems` -> Array<UniquePtr<Animation>> (RAII single-owner). Duration is computed from
// children (base(0)); Storyboard overrides Update directly so Apply is a no-op.

module;
#include "Core/Prelude.h"

export module foundation.ui:storyboard;

import foundation.core;
import :animation;

using namespace foundation::core;

export namespace foundation::ui
{
    class Storyboard : public Animation
    {
    public:
        enum class Mode
        {
            /// Play animations one after another.
            Sequential,
            /// Play all animations at the same time.
            Parallel
        };

        explicit Storyboard(Mode mode) : Animation(0), m_mode(mode) {}

        /// Add a child animation. The Storyboard takes ownership.
        void Add(UniquePtr<Animation> anim) { m_children.PushBack(Move(anim)); }

        /// Number of child animations.
        [[nodiscard]] usize ChildCount() const noexcept { return m_children.Size(); }

        bool Update(f32 deltaTime) override
        {
            if (!IsRunning() || IsComplete())
            {
                return IsComplete();
            }

            if (m_children.Size() == 0)
            {
                MarkComplete();
                return true;
            }

            switch (m_mode)
            {
            case Mode::Sequential:
                return UpdateSequential(deltaTime);
            case Mode::Parallel:
                return UpdateParallel(deltaTime);
            }
            return false;
        }

        /// Reset this storyboard and all children.
        void Reset() override
        {
            Animation::Reset();
            m_currentIndex = 0;
            for (const UniquePtr<Animation>& child : m_children)
            {
                child->Reset();
            }
        }

    protected:
        void Apply(f32) override { /* Not used - Storyboard overrides Update directly. */ }

    private:
        bool UpdateSequential(f32 deltaTime)
        {
            while (m_currentIndex < m_children.Size())
            {
                Animation* child = m_children[m_currentIndex].Get();
                if (!child->IsRunning() && !child->IsComplete())
                {
                    child->Start();
                }

                if (child->Update(deltaTime))
                {
                    m_currentIndex++;
                    continue;
                }
                return false; // Current child still running.
            }

            // All children complete.
            MarkComplete();
            return true;
        }

        bool UpdateParallel(f32 deltaTime)
        {
            bool allDone = true;
            for (const UniquePtr<Animation>& child : m_children)
            {
                if (!child->IsRunning() && !child->IsComplete())
                {
                    child->Start();
                }
                if (!child->Update(deltaTime))
                {
                    allDone = false;
                }
            }

            if (allDone)
            {
                MarkComplete();
                return true;
            }
            return false;
        }

        Mode m_mode;
        Array<UniquePtr<Animation>> m_children;
        usize m_currentIndex = 0;
    };
}
