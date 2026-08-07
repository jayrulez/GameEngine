// draconic.engine.integration tests: the physics-contact -> script bridge in isolation.
// The kind mapping is pure (no subsystems needed); the full contact->behavior delivery through
// this SAME bridge is covered end-to-end by the ContactWorld battery in the script scene tests.

#include <doctest/doctest.h>

#include "Draconic.Foundation/Prelude.h"

import draconic.foundation;
import draconic.physics;         // ContactKind
import draconic.engine.script;   // ScriptContactKind
import draconic.engine.integration;

using namespace draconic::foundation;
using draconic::integration::ScriptPhysicsContactBridge;
using draconic::integration::ToScriptContactKind;
using CK = draconic::physics::ContactKind;
using SK = draconic::script::ScriptContactKind;

TEST_CASE("integration: ToScriptContactKind maps every physics contact kind onto its script kind")
{
    CHECK(ToScriptContactKind(CK::Begin) == SK::Begin);
    CHECK(ToScriptContactKind(CK::End) == SK::End);
    CHECK(ToScriptContactKind(CK::TriggerEnter) == SK::TriggerEnter);
    CHECK(ToScriptContactKind(CK::TriggerExit) == SK::TriggerExit);
}

TEST_CASE("integration: a fresh bridge is not installed and Uninstall is a safe no-op")
{
    ScriptPhysicsContactBridge bridge;
    CHECK_FALSE(bridge.Installed());
    bridge.Uninstall(); // idempotent when never installed
    CHECK_FALSE(bridge.Installed());
    // Destructor of an un-installed bridge must not touch a (null) physics subsystem - it just
    // goes out of scope here without a crash.
}
