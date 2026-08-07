// Draconic GUI - :action_manager partition
//
// ActionManager: owns and ticks running Actions. Ported from eepp's Scene::ActionManager
// (include/eepp/scene/actionmanager.hpp). Owned by the SceneNode; Node::RunAction forwards
// here. Actions are RefPtr-owned; a finished action is dropped after Update.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.gui:action_manager;

import draconic.foundation; // RefPtr, Array, Duration, Move
import :action;

using namespace draconic::foundation;
namespace foundation = draconic::foundation;

export namespace draconic::gui
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
            m_actions.PushBack(foundation::Move(action));
        }

        // Ticks all actions; drops finished ones.
        void Update(foundation::Duration elapsed)
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
