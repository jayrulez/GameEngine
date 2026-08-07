// Draconic::EngineIntegration - implementation unit: the bridge bodies. The virtual
// IContactListener override + the subsystem calls live OUTSIDE the interface (GCC module
// hygiene: cross-partition inline virtuals are not reliably emitted).

module;
#include "Draconic.Foundation/Prelude.h"

module draconic.engine.integration;

import draconic.foundation;
import draconic.physics;
import draconic.engine.physics;
import draconic.engine.script;

namespace draconic::integration
{
    draconic::script::ScriptContactKind
    ToScriptContactKind(draconic::physics::ContactKind kind) noexcept
    {
        using SK = draconic::script::ScriptContactKind;
        switch (kind)
        {
        case draconic::physics::ContactKind::Begin:
            return SK::Begin;
        case draconic::physics::ContactKind::End:
            return SK::End;
        case draconic::physics::ContactKind::TriggerEnter:
            return SK::TriggerEnter;
        case draconic::physics::ContactKind::TriggerExit:
            return SK::TriggerExit;
        }
        return SK::Begin;
    }

    void ScriptPhysicsContactBridge::Listener::OnContact(const draconic::physics::EntityContact& c)
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

    void ScriptPhysicsContactBridge::Install(draconic::physics::PhysicsSubsystem& physics,
                                             draconic::script::ScriptSubsystem& scripts)
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
