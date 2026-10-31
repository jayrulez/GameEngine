// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUISandbox - what the editor mock's inspectors show: an entity with three components and a
// scene's settings, reflected as the engine's are, standing in for them.
// Plain data: nothing here runs.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module gui.sandbox.mock_types;

import foundation.core;

using namespace foundation::core;

export namespace sandbox
{
    enum class MockMotion : u8
    {
        Dynamic,
        Kinematic,
        Static,
    };
    enum class MockTonemap : u8
    {
        Aces,
        AgX,
        Neutral,
    };

    struct MockTransform
    {
        Float3 position{2.0f, 0.5f, -1.0f};
        Quaternion rotation = Quaternion::Identity;
        Float3 scale{1.0f, 1.0f, 1.0f};
    };
    struct MockMeshRenderer
    {
        String mesh = String(u8"Crate");
        String material = String(u8"Wood");
        bool castShadows = true;
        bool receiveShadows = true;
    };
    struct MockRigidBody
    {
        MockMotion motion = MockMotion::Dynamic;
        f32 mass = 12.0f;
        f32 linearDamping = 0.05f;
        f32 friction = 0.6f;
        bool startAsleep = false;
    };
    /// An entity: its name and the components it has.
    class MockEntity : public Object
    {
        RTTI_OBJECT(MockEntity, Object)
    public:
        String name = String(u8"Crate");
        bool enabled = true;
        MockTransform transform;
        MockMeshRenderer meshRenderer;
        MockRigidBody rigidBody;
    };

    struct MockEnvironment
    {
        Color skyColor{0.42f, 0.6f, 0.9f, 1.0f};
        f32 ambientIntensity = 0.8f;
        bool fog = true;
        f32 fogDensity = 0.02f;
    };
    struct MockRendering
    {
        f32 exposure = 0.0f;
        MockTonemap tonemap = MockTonemap::AgX;
        bool shadows = true;
        f32 shadowDistance = 60.0f;
    };
    struct MockPhysics
    {
        Float3 gravity{0.0f, -9.81f, 0.0f};
        i32 substeps = 2;
    };
    struct MockAudio
    {
        f32 masterVolume = 0.9f;
        f32 dopplerScale = 1.0f;
    };
    /// A scene's own settings: what its inspector shows when the scene itself is selected.
    class MockScene : public Object
    {
        RTTI_OBJECT(MockScene, Object)
    public:
        MockEnvironment environment;
        MockRendering rendering;
        MockPhysics physics;
        MockAudio audio;
    };

    /// Registers the mock's reflection (once).
    void RegisterMockTypes();
}
