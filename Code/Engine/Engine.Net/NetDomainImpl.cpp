// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell
// Engine::Net - the net domain's declaration, in an implementation unit so the module is one
// instance per process.
module;
#include "Core/Prelude.h"

module engine.net;

import foundation.core;
import foundation.net.manager;
import foundation.net.replication;
import engine.domain;

using namespace foundation::core;

namespace engine::net
{
    void RegisterNetworkScriptFacades()
    {
        foundation::net::RegisterNetScriptFacade();
        foundation::net::RegisterNetworkComponentScriptFacade();
    }

    const engine::DomainModule& NetDomain() noexcept
    {
        static const engine::DomainModule kModule{
            .id = u8"net",
            .installScene = &AddNetworkSceneManagers,
            .registerReflection = &foundation::net::RegisterReplicationComponents,
            .registerScriptFacade = &RegisterNetworkScriptFacades};
        return kModule;
    }
}
