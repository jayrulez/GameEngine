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

        // Ticks all actions; drops finished ones.
        void Update(core::Duration elapsed)
        {
            for (usize i = 0; i < m_actions.Size();)
            {
                m_actions[i]->Update(elapsed);
                if (m_actions[i]->IsDone())
                    m_actions.RemoveAt(i);
                else
                    ++i;
            }
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
