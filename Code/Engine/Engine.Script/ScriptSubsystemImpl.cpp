// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Engine::Script - implementation unit: the reflection bodies + the
// component-destroy hook. They live OUTSIDE the interface for GCC: REFLECT_*
// bodies in a module interface make GCC emit an unreadable gcm cluster for
// -fno-module-lazy consumers (and a cross-partition inline virtual is not reliably
// emitted by either compiler).

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

module engine.script;
import engine.domain;

import foundation.core;
import foundation.scene;
import foundation.resource; // resource::Ref (SceneScriptSettings.script)
import foundation.script;
import foundation.script.resource;

using namespace foundation::core;

namespace engine::script
{
    // Destroying an entity (or removing the component) delivers onDestroy through the
    // scene's script system before the instances are dropped.
    void ScriptComponentManager::OnComponentDestroyed(ScriptComponent& component,
                                                      foundation::scene::EntityHandle entity)
    {
        if (m_scriptSystem != nullptr)
        {
            m_scriptSystem->ReleaseComponentInstances(component, entity);
        }
        else
        {
            for (ScriptBehavior& behavior : component.behaviors)
            {
                behavior.instance = nullptr;
                behavior.boundClass = nullptr;
            }
        }
    }

    // The component itself carries no inspector-editable reflected properties (its
    // behavior array renders through the bespoke inspector section), but it MUST be
    // reflected so the Add Component menu lists it and versioned payloads carry a
    // data version.
    REFLECT_VALUE(ScriptComponent, "rtti::engine::script")
    {
        builder.Attribute("displayName", String(u8"Script"))
            .Attribute("category", String(u8"Scripting"))
            .Attribute("description",
                       String(u8"Runs scripts on the entity, in order, each with its own "
                              u8"property values."))
            .DataVersion(1);
    }

    // The scene-root script block (one per scene). Reflected so the scene-settings inspector renders
    // its Level-script Ref picker + enable toggle, and so versioned payloads carry a data version.
    REFLECT_VALUE(SceneScriptSettings, "rtti::engine::script")
    {
        builder.Attribute("displayName", String(u8"Scene Script"))
            .Attribute("category", String(u8"Scripting"))
            .DataVersion(2) // v2: hash-keyed Level property overrides
            .Property<&SceneScriptSettings::script>("script")
            .PropAttribute("displayName", String(u8"Level Script"))
            .PropAttribute("description",
                           String(u8"A script class with onStart/onUpdate/onFixedUpdate/onStop, "
                                  u8"instantiated once per scene"))
            .Property<&SceneScriptSettings::enabled>("enabled")
            .PropAttribute("displayName", String(u8"Enabled"));
    }

    void RegisterScriptComponentReflection()
    {
        static const bool once = []()
        {
            RttiRegisterValue_ScriptComponent();
            GlobalTypeRegistry().Register(TypeOf<ScriptComponent>());
            RttiRegisterValue_SceneScriptSettings();
            GlobalTypeRegistry().Register(TypeOf<SceneScriptSettings>());
            return true;
        }();
        (void)once;
    }
}

namespace engine::script
{
    REFLECT_VALUE(SceneScripts, "rtti::engine::script")
    {
        builder.Method<&SceneScripts::deltaTime>("deltaTime");
        builder.Method<&SceneScripts::elapsed>("elapsed");
        // send and emit - ARITY FAMILIES with a Variant payload, as entity.send / scene.events.emit.
        builder.Method<static_cast<void (SceneScripts::*)(foundation::script::Entity, String) const>(&SceneScripts::send)>(
            "send", {"target", "message"});
        builder.Method<static_cast<void (SceneScripts::*)(foundation::script::Entity, String, Variant) const>(
            &SceneScripts::send)>("send", {"target", "message", "payload"});
        builder.Method<static_cast<void (SceneScripts::*)(String) const>(&SceneScripts::emit)>(
            "emit", {"name"});
        builder.Method<static_cast<void (SceneScripts::*)(String, Variant) const>(&SceneScripts::emit)>(
            "emit", {"name", "payload"});
        builder.Method<&SceneScripts::addBehavior>("addBehavior", {"entity", "scriptClass"});
        builder.Method<&SceneScripts::of>("of", {"scene"});
        builder.Constructor(); // some backends only materialize constructible foreign classes
    }

    void RegisterScriptSceneFacade()
    {
        static const bool once = []()
        {
            RttiRegisterValue_SceneScripts();
            GlobalTypeRegistry().Register(TypeOf<SceneScripts>());
            foundation::script::RegisterExtraScriptRootType(&TypeOf<SceneScripts>());
            foundation::script::RegisterExtraFacadeName(u8"SceneScripts");
            return true;
        }();
        (void)once;
    }

    const engine::DomainModule& ScriptDomain() noexcept
    {
        static const foundation::resource::ResourceModule* const kResources[] = {
            &foundation::script::kScriptResourceModule};
        static const engine::DomainModule kModule{
            .id = u8"script",
            .installScene = &AddScriptSceneManagers,
            .registerReflection = &RegisterScriptComponentReflection,
            .registerScriptFacade = &RegisterScriptSceneFacade,
            .resources = foundation::core::Span<const foundation::resource::ResourceModule* const>{
                kResources, sizeof(kResources) / sizeof(kResources[0])}};
        return kModule;
    }
}
