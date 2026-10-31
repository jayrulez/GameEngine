// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Engine::Integration - implementation unit: the bridge bodies. The virtual IContactListener
// override and the subsystem calls live outside the interface, because GCC does not reliably emit
// cross-partition inline virtuals.

module;
#include "Core/Prelude.h"

module engine.integration;

import foundation.core;
import foundation.physics;
import engine.physics;
import engine.script;

namespace engine::integration
{
    engine::script::ScriptContactKind
    ToScriptContactKind(foundation::physics::ContactKind kind) noexcept
    {
        using SK = engine::script::ScriptContactKind;
        switch (kind)
        {
        case foundation::physics::ContactKind::Begin:
            return SK::Begin;
        case foundation::physics::ContactKind::End:
            return SK::End;
        case foundation::physics::ContactKind::TriggerEnter:
            return SK::TriggerEnter;
        case foundation::physics::ContactKind::TriggerExit:
            return SK::TriggerExit;
        }
        return SK::Begin;
    }

    void ScriptPhysicsContactBridge::Listener::OnContact(const engine::physics::EntityContact& c)
    {
        if (scripts == nullptr)
        {
            return;
        }
        scripts->DeliverContact(c.scene, c.a, c.b, ToScriptContactKind(c.kind), c.point, c.normal,
                                c.speed);
    }

    ScriptPhysicsContactBridge::~ScriptPhysicsContactBridge()
    {
        Uninstall();
    }

    void ScriptPhysicsContactBridge::Install(engine::physics::PhysicsSubsystem& physics,
                                             engine::script::ScriptSubsystem& scripts)
    {
        Uninstall(); // drop any prior registration so a re-Install re-points cleanly
        m_listener.scripts = &scripts;
        m_physics = &physics;
        m_physics->RegisterContactListener(&m_listener);
    }

    void ScriptPhysicsContactBridge::Uninstall()
    {
        if (m_physics != nullptr)
        {
            m_physics->UnregisterContactListener(&m_listener);
            m_physics = nullptr;
        }
        m_listener.scripts = nullptr;
    }
}
