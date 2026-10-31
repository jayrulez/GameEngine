// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::ViewportTools - implementation unit (the manager's state machine and the provider
// registry).

module;
#include "Core/Prelude.h"

module editor.viewporttools;

import foundation.core;

using namespace foundation::core;

namespace editor
{
    IViewportTool* ViewportToolManager::Add(UniquePtr<IViewportTool> tool)
    {
        if (tool.Get() == nullptr)
        {
            return nullptr;
        }
        IViewportTool* raw = tool.Get();
        m_tools.PushBack(Move(tool));
        if (m_active == nullptr)
        {
            m_active = raw; // the first tool is the default and starts active
            m_active->OnActivate();
        }
        return raw;
    }

    IViewportTool* ViewportToolManager::ToolAt(usize index) noexcept
    {
        return index < m_tools.Size() ? m_tools[index].Get() : nullptr;
    }

    IViewportTool* ViewportToolManager::FindById(StringView id) noexcept
    {
        for (UniquePtr<IViewportTool>& tool : m_tools)
        {
            if (tool->Id() == id)
            {
                return tool.Get();
            }
        }
        return nullptr;
    }

    bool ViewportToolManager::ActivateById(StringView id)
    {
        IViewportTool* target = FindById(id);
        if (target == nullptr || !target->IsAvailable())
        {
            return false;
        }
        if (target == m_active)
        {
            return true;
        }
        if (m_active != nullptr)
        {
            m_active->OnDeactivate(); // ends any gesture before the switch
        }
        m_active = target;
        m_active->OnActivate();
        return true;
    }

    void ViewportToolManager::ActivateDefault()
    {
        if (m_tools.IsEmpty() || m_active == m_tools[0].Get())
        {
            return;
        }
        if (m_active != nullptr)
        {
            m_active->OnDeactivate();
        }
        m_active = m_tools[0].Get();
        m_active->OnActivate();
    }

    bool ViewportToolManager::Update(const ViewportToolInput& input)
    {
        if (m_active == nullptr)
        {
            return false;
        }
        // Availability depends on context and can lapse mid-session (the terrain was deleted),
        // so fall back before routing; an unavailable tool never sees another frame.
        if (!m_active->IsAvailable())
        {
            ActivateDefault();
        }
        return m_active != nullptr && m_active->Update(input);
    }

    void ViewportToolManager::Draw(foundation::render::debug::DebugDraw& drawList)
    {
        if (m_active != nullptr)
        {
            m_active->Draw(drawList);
        }
    }

    ViewportToolProviderRegistry& ViewportToolProviderRegistry::Get()
    {
        static ViewportToolProviderRegistry instance;
        return instance;
    }

    void ViewportToolProviderRegistry::Register(IViewportToolProvider* provider)
    {
        if (provider == nullptr)
        {
            return;
        }
        for (IViewportToolProvider* existing : m_providers)
        {
            if (existing == provider)
            {
                return; // idempotent re-registration
            }
        }
        m_providers.PushBack(provider);
    }

    void ViewportToolProviderRegistry::CreateAll(ViewportToolManager& manager,
                                                 const ViewportToolHostContext& context)
    {
        for (IViewportToolProvider* provider : m_providers)
        {
            provider->CreateTools(manager, context);
        }
    }

    void GroupViewportTools(ViewportToolManager& manager, Array<ViewportToolGroup>& out)
    {
        for (usize i = 1; i < manager.Count(); ++i)
        {
            IViewportTool* tool = manager.ToolAt(i);
            if (tool == nullptr)
            {
                continue;
            }
            const StringView category = tool->Category();
            ViewportToolGroup* group = nullptr;
            if (!category.IsEmpty())
            {
                for (ViewportToolGroup& g : out)
                {
                    if (g.category.AsView() == category)
                    {
                        group = &g;
                        break;
                    }
                }
            }
            if (group == nullptr)
            {
                ViewportToolGroup fresh;
                fresh.category = String(category);
                out.PushBack(Move(fresh));
                group = &out.Back();
            }
            group->toolIds.PushBack(String(tool->Id()));
        }
    }
}
