// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Engine::Composition - the :plugin_services implementation.

module;
#include "Core/Prelude.h"

module engine.composition;

import foundation.core;
import foundation.runtime;
import foundation.scene;
import engine.domain;
import engine.scene; // GlobalSceneContributionRecorder

using namespace foundation::core;

namespace engine
{
    bool EnginePluginServices::Attach(foundation::runtime::PluginHost& host)
    {
        if (!host.Provide(*this))
        {
            return false;
        }
        host.Loader().AddRecorder(this);
        host.Loader().AddRecorder(&engine::scene::GlobalSceneContributionRecorder());
        return true;
    }

    bool EnginePluginServices::AddDomain(const DomainModule& module)
    {
        const u64 token = AddPluginDomain(module);
        if (token == 0)
        {
            return false;
        }
        const u64 recorded = m_nextToken++;
        m_entries.PushBack(Entry{recorded, token});
        if (m_sink != nullptr)
        {
            m_sink->PushBack(recorded);
        }
        return true;
    }

    void EnginePluginServices::AddSceneManager(
        const foundation::scene::SceneModuleContributions::Contribution& contribution)
    {
        // The scene contribution recorder the host armed records it; it is reversed with the rest.
        foundation::scene::SceneModuleContributions::Global().Add(contribution);
    }

    void EnginePluginServices::Reverse(TypeId id)
    {
        for (usize i = 0; i < m_entries.Size(); ++i)
        {
            if (m_entries[i].token != id)
            {
                continue;
            }
            const u64 domain = m_entries[i].domain;
            m_entries.RemoveAt(i);
            RemovePluginDomain(domain);
            return;
        }
    }
}
