// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - module implementation unit: ActionManager's tick, which needs the full Node type that the
// partition only names.

module;
#include "Core/Prelude.h"

module foundation.gui;

using namespace foundation::core;
namespace core = foundation::core;

namespace foundation::gui
{
    void ActionManager::Update(core::Duration elapsed, const TimeScale& scaleOf)
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
            // While its target is out of view, an action that cannot bring it into view changes
            // nothing visible, so its writes mark only the target and do not ask for a frame.
            const bool quiet = target != nullptr && target->WasOutOfView() && !m_actions[i]->CanMoveTarget();
            if (quiet)
                target->BeginQuietInvalidation();
            if (scale == 1.0f)
                m_actions[i]->Update(elapsed);
            else if (scale > 0.0f)
                m_actions[i]->Update(core::Duration::FromSeconds(static_cast<f64>(elapsed.AsSecondsF()) * static_cast<f64>(scale)));
            if (quiet)
                target->EndQuietInvalidation();
            if (m_actions[i]->IsDone())
                m_actions.RemoveAt(i);
            else
                ++i;
        }
    }
}
