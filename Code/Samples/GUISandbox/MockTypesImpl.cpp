// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUISandbox - the editor mock's types' reflection.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

module gui.sandbox.mock_types;

import foundation.core;

using namespace foundation::core;

namespace sandbox
{
    REFLECT_ENUM(MockMotion, "rtti::sandbox")
    {
        builder.Value("Dynamic", MockMotion::Dynamic);
        builder.Value("Kinematic", MockMotion::Kinematic);
        builder.Value("Static", MockMotion::Static);
    }
    REFLECT_ENUM(MockTonemap, "rtti::sandbox")
    {
        builder.Value("ACES", MockTonemap::Aces);
        builder.Value("AgX", MockTonemap::AgX);
        builder.Value("Neutral", MockTonemap::Neutral);
    }
    REFLECT_VALUE(MockTransform, "rtti::sandbox")
    {
        builder.Property<&MockTransform::position>("position");
        builder.Property<&MockTransform::rotation>("rotation");
        builder.Property<&MockTransform::scale>("scale");
    }
    REFLECT_VALUE(MockMeshRenderer, "rtti::sandbox")
    {
        builder.Property<&MockMeshRenderer::mesh>("mesh");
        builder.Property<&MockMeshRenderer::material>("material");
        builder.Property<&MockMeshRenderer::castShadows>("castShadows");
        builder.Property<&MockMeshRenderer::receiveShadows>("receiveShadows");
    }
    REFLECT_VALUE(MockRigidBody, "rtti::sandbox")
    {
        builder.Property<&MockRigidBody::motion>("motion");
        builder.Property<&MockRigidBody::mass>("mass").PropAttribute("range", Float4{0.0f, 1000.0f, 0.1f, 0.0f});
        builder.Property<&MockRigidBody::linearDamping>("linearDamping").PropAttribute("range", Float4{0.0f, 1.0f, 0.01f, 0.0f});
        builder.Property<&MockRigidBody::friction>("friction").PropAttribute("range", Float4{0.0f, 1.0f, 0.01f, 0.0f});
        builder.Property<&MockRigidBody::startAsleep>("startAsleep");
    }
    REFLECT_MEMBERS(MockEntity, "rtti::sandbox")
    {
        builder.Property<&MockEntity::name>("name");
        builder.Property<&MockEntity::enabled>("enabled");
        builder.Property<&MockEntity::transform>("transform");
        builder.Property<&MockEntity::meshRenderer>("meshRenderer");
        builder.Property<&MockEntity::rigidBody>("rigidBody");
        builder.Creator();
    }

    REFLECT_VALUE(MockEnvironment, "rtti::sandbox")
    {
        builder.Property<&MockEnvironment::skyColor>("skyColor");
        builder.Property<&MockEnvironment::ambientIntensity>("ambientIntensity").PropAttribute("range", Float4{0.0f, 4.0f, 0.05f, 0.0f});
        builder.Property<&MockEnvironment::fog>("fog");
        builder.Property<&MockEnvironment::fogDensity>("fogDensity")
            .PropAttribute("range", Float4{0.0f, 0.2f, 0.001f, 0.0f})
            .PropAttribute("visibleWhen", String(u8"fog=1"));
    }
    REFLECT_VALUE(MockRendering, "rtti::sandbox")
    {
        builder.Property<&MockRendering::exposure>("exposure").PropAttribute("range", Float4{-8.0f, 8.0f, 0.1f, 0.0f});
        builder.Property<&MockRendering::tonemap>("tonemap");
        builder.Property<&MockRendering::shadows>("shadows");
        builder.Property<&MockRendering::shadowDistance>("shadowDistance").PropAttribute("range", Float4{1.0f, 500.0f, 1.0f, 0.0f});
    }
    REFLECT_VALUE(MockPhysics, "rtti::sandbox")
    {
        builder.Property<&MockPhysics::gravity>("gravity");
        builder.Property<&MockPhysics::substeps>("substeps").PropAttribute("range", Float4{1.0f, 8.0f, 1.0f, 0.0f});
    }
    REFLECT_VALUE(MockAudio, "rtti::sandbox")
    {
        builder.Property<&MockAudio::masterVolume>("masterVolume").PropAttribute("range", Float4{0.0f, 1.0f, 0.01f, 0.0f});
        builder.Property<&MockAudio::dopplerScale>("dopplerScale").PropAttribute("range", Float4{0.0f, 4.0f, 0.05f, 0.0f});
    }
    REFLECT_MEMBERS(MockScene, "rtti::sandbox")
    {
        builder.Property<&MockScene::environment>("environment");
        builder.Property<&MockScene::rendering>("rendering");
        builder.Property<&MockScene::physics>("physics");
        builder.Property<&MockScene::audio>("audio");
        builder.Creator();
    }

    void RegisterMockTypes()
    {
        static const bool once = []()
        {
            RttiRegisterEnum_MockMotion();
            RttiRegisterEnum_MockTonemap();
            RttiRegisterValue_MockTransform();
            RttiRegisterValue_MockMeshRenderer();
            RttiRegisterValue_MockRigidBody();
            RttiRegisterValue_MockEnvironment();
            RttiRegisterValue_MockRendering();
            RttiRegisterValue_MockPhysics();
            RttiRegisterValue_MockAudio();
            GlobalTypeRegistry().Register(MockEntity::StaticType());
            GlobalTypeRegistry().Register(MockScene::StaticType());
            return true;
        }();
        (void)once;
    }
}
