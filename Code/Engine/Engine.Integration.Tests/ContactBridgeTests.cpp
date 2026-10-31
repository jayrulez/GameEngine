// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// engine.integration tests: the physics-contact-to-script bridge in isolation. The kind mapping is
// pure (no subsystems needed); the full contact-to-behavior delivery through this same bridge is
// covered end to end by the ContactWorld tests in the script scene tests.

#include <doctest/doctest.h>

#include "Core/Prelude.h"

import foundation.core;
import foundation.physics;         // ContactKind
import engine.script;   // ScriptContactKind
import engine.integration;

using namespace foundation::core;
using engine::integration::ScriptPhysicsContactBridge;
using engine::integration::ToScriptContactKind;
using CK = foundation::physics::ContactKind;
using SK = engine::script::ScriptContactKind;

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
    // Destroying an uninstalled bridge must not touch a (null) physics subsystem; it goes out of
    // scope here without a crash.
}
