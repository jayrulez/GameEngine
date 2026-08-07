// Draconic::EngineIntegration - the `draconic.engine.integration` module.
//
// Cross-subsystem composition helpers: the small adapters that let two otherwise-independent
// subsystems cooperate, owned by a composition root (the app) and depending on BOTH sides so
// neither subsystem gains a dependency on the other.
//
// First tenant: ScriptPhysicsContactBridge - forwards physics contacts to the script
// subsystem's neutral DeliverContact ingress. Physics exposes IContactListener (knows nothing
// about scripts); the script subsystem exposes DeliverContact in its own vocabulary (knows
// nothing about physics); this bridge is the ONE place that names both and translates between
// them, so draconic.engine.physics and draconic.engine.script stay mutually independent.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.engine.integration;

import draconic.foundation;
import draconic.physics;        // ContactKind (the foundation contact enum)
import draconic.engine.physics; // PhysicsSubsystem, IContactListener, EntityContact
import draconic.engine.script;  // ScriptSubsystem, ScriptContactKind

export namespace draconic::integration
{
    /// Maps the physics contact vocabulary onto the script subsystem's neutral one. Free +
    /// pure so it is trivially testable without either subsystem.
    [[nodiscard]] draconic::script::ScriptContactKind
    ToScriptContactKind(draconic::physics::ContactKind kind) noexcept;

    /// Registers a physics contact listener that forwards resolved contacts to a script
    /// subsystem, translating the contact kind. The composition root (the app) owns one of
    /// these and Install()s it once the physics + script subsystems exist; it Uninstall()s on
    /// teardown (the destructor also does, so a dropped bridge never dangles in the physics
    /// listener list).
    class ScriptPhysicsContactBridge
    {
    public:
        ScriptPhysicsContactBridge() = default;
        ~ScriptPhysicsContactBridge();

        ScriptPhysicsContactBridge(const ScriptPhysicsContactBridge&) = delete;
        ScriptPhysicsContactBridge& operator=(const ScriptPhysicsContactBridge&) = delete;

        /// Point `physics` contacts at `scripts`. Calling again re-points cleanly (it
        /// Uninstall()s the prior registration first), so it is safe to re-wire.
        void Install(draconic::physics::PhysicsSubsystem& physics,
                     draconic::script::ScriptSubsystem& scripts);

        /// Unregister from the physics subsystem. Idempotent; a no-op when not installed.
        void Uninstall();

        [[nodiscard]] bool Installed() const noexcept { return m_physics != nullptr; }

    private:
        struct Listener final : public draconic::physics::IContactListener
        {
            draconic::script::ScriptSubsystem* scripts = nullptr;
            void OnContact(const draconic::physics::EntityContact& contact) override;
        };

        Listener m_listener;
        draconic::physics::PhysicsSubsystem* m_physics = nullptr;
    };
}
