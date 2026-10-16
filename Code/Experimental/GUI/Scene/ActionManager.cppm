// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :action_manager partition
//
// ActionManager: owns and ticks running Actions. Ported from eepp's Scene::ActionManager
// (include/eepp/scene/actionmanager.hpp). Owned by the SceneRoot; Node::RunAction forwards
// here. Actions are RefPtr-owned; a finished action is dropped after Update.

module;
#include "Core/Prelude.h"

export module experimental.gui:action_manager;

import foundation.core; // RefPtr, Array, Duration, Move
import :action;

using namespace foundation::core;
namespace core = foundation::core;

export namespace experimental::gui
{
    class Node;

    class ActionManager
    {
    public:
        // Adds and starts an action.
        void AddAction(RefPtr<Action> action)
        {
            if (!action)
                return;
            action->Start();
            m_actions.PushBack(core::Move(action));
        }

        /// How fast an action's time runs (its target's root may be paused, 0, or slowed).
        using TimeScale = core::Function<f32(Node& target)>;

        // Ticks all actions, each by `elapsed` times its target's time scale (1 without one);
        // drops finished ones and those whose target is gone.
        void Update(core::Duration elapsed, const TimeScale& scaleOf = {})
        {
            for (usize i = 0; i < m_actions.Size();)
            {
                Node* target = m_actions[i]->ResolveTarget();
                if (target == nullptr && m_actions[i]->GetTarget() != nullptr)
                {
                    m_actions.RemoveAt(i); // its target was destroyed
                    continue;
                }
                const f32 scale = (scaleOf && target != nullptr) ? scaleOf(*target) : 1.0f;
                if (scale == 1.0f)
                    m_actions[i]->Update(elapsed);
                else if (scale > 0.0f)
                    m_actions[i]->Update(core::Duration::FromSeconds(static_cast<f64>(elapsed.AsSecondsF()) * static_cast<f64>(scale)));
                if (m_actions[i]->IsDone())
                    m_actions.RemoveAt(i);
                else
                    ++i;
            }
        }

        /// Whether an action is running on a target `matches` takes (a root's own, for its frames).
        template <typename Predicate>
        [[nodiscard]] bool AnyTarget(Predicate&& matches) const
        {
            for (const RefPtr<Action>& a : m_actions)
                if (Node* target = a->ResolveTarget(); target != nullptr && matches(*target))
                    return true;
            return false;
        }

        bool RemoveAllActionsFromTarget(Node* target)
        {
            bool removed = false;
            for (usize i = 0; i < m_actions.Size();)
            {
                if (m_actions[i]->GetTarget() == target)
                {
                    m_actions.RemoveAt(i);
                    removed = true;
                }
                else
                    ++i;
            }
            return removed;
        }

        [[nodiscard]] Action* GetActionByTag(u64 tag) const
        {
            for (const RefPtr<Action>& a : m_actions)
                if (a->GetTag() == tag)
                    return a.Get();
            return nullptr;
        }

        [[nodiscard]] usize Count() const noexcept { return m_actions.Size(); }
        [[nodiscard]] bool IsEmpty() const noexcept { return m_actions.Size() == 0; }
        void Clear() { m_actions.Clear(); }

    private:
        Array<RefPtr<Action>> m_actions;
    };
}
