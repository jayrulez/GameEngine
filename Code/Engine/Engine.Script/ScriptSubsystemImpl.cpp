// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Engine::Script - implementation unit: the reflection bodies and the
// component-destroy hook. They live outside the interface for GCC: REFLECT_*
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
using namespace foundation::script; // ScriptPropertyDesc, ScriptPropertyValue, ScriptClass (the replicated state)

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
    // behavior array renders through the bespoke inspector section), but it must be
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
    // its Level-script Ref picker and enable toggle, and so versioned payloads carry a data version.
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

    namespace
    {
        // A script's live property value, as the property's own kind (numbers come back as f64).
        [[nodiscard]] bool VariantToPropertyValue(const Variant& value, ScriptPropertyType type,
                                                  ScriptPropertyValue& out)
        {
            out.kind = type;
            switch (type)
            {
            case ScriptPropertyType::Float:
            case ScriptPropertyType::Int:
                if (const f64* n = value.TryGet<f64>())
                {
                    out.number = *n;
                    return true;
                }
                if (const f32* f = value.TryGet<f32>())
                {
                    out.number = *f;
                    return true;
                }
                if (const i32* i = value.TryGet<i32>())
                {
                    out.number = *i;
                    return true;
                }
                return false;
            case ScriptPropertyType::Bool:
                if (const bool* b = value.TryGet<bool>())
                {
                    out.boolean = *b;
                    return true;
                }
                return false;
            case ScriptPropertyType::String:
                if (const String* t = value.TryGet<String>())
                {
                    out.text = *t;
                    return true;
                }
                return false;
            case ScriptPropertyType::Color:
                if (const Color* c = value.TryGet<Color>())
                {
                    out.color = *c;
                    return true;
                }
                return false;
            case ScriptPropertyType::Vec3:
                if (const Float3* v = value.TryGet<Float3>())
                {
                    out.vector = *v;
                    return true;
                }
                return false;
            default:
                return false; // entity and asset references are never replicated (refused at cook)
            }
        }

        // One replicated property of one behaviour, as the bytes carry it.
        struct ReplicatedEntry
        {
            u32 behavior = 0;
            u64 classHash = 0; // which class the behaviour was, so a mismatch is never applied
            u64 property = 0;
            ScriptPropertyValue value;
        };

        void Serialize(ISerializer& ar, ReplicatedEntry& e)
        {
            foundation::core::Serialize(ar, "behavior", e.behavior);
            foundation::core::Serialize(ar, "class", e.classHash);
            foundation::core::Serialize(ar, "property", e.property);
            foundation::core::Serialize(ar, "value", e.value);
        }
    }

    bool ScriptComponentManager::WriteReplicatedState(foundation::scene::EntityHandle entity, Array<byte>& out)
    {
        const ScriptComponent* component = Get(entity);
        if (component == nullptr)
        {
            return false;
        }
        Array<ReplicatedEntry> entries;
        for (usize b = 0; b < component->behaviors.Size(); ++b)
        {
            const ScriptBehavior& behavior = component->behaviors[b];
            const ScriptClass* scriptClass = behavior.script.Get();
            if (scriptClass == nullptr)
            {
                continue;
            }
            for (const ScriptPropertyDesc& property : scriptClass->properties)
            {
                if (!property.replicated)
                {
                    continue;
                }
                ReplicatedEntry entry;
                entry.behavior = static_cast<u32>(b);
                entry.classHash = ScriptPropertyNameHash(scriptClass->className.AsView());
                entry.property = property.hash;
                // The live value when the script runs; else what it would start with.
                bool live = false;
                if (behavior.instance.Get() != nullptr)
                {
                    const Result<Variant> current = behavior.instance->GetProperty(property.name.AsView());
                    live = current.HasValue() && VariantToPropertyValue(current.Value(), property.type, entry.value);
                }
                if (!live)
                {
                    const ScriptPropertyOverride* over = behavior.FindOverride(property.hash);
                    entry.value = over != nullptr ? over->value : property.defaultValue;
                    entry.value.kind = property.type;
                }
                entries.PushBack(Move(entry));
            }
        }
        if (entries.IsEmpty())
        {
            return false;
        }
        MemoryStream stream;
        {
            BinarySerializer writer(stream, SerializeMode::Write);
            foundation::core::Serialize(writer, "entries", entries);
        }
        out.Clear();
        for (const byte b : stream.Bytes())
        {
            out.PushBack(b);
        }
        return true;
    }

    void ScriptComponentManager::ReadReplicatedState(foundation::scene::EntityHandle entity, Span<const byte> state)
    {
        ScriptComponent* component = Get(entity);
        if (component == nullptr)
        {
            return;
        }
        MemoryStream stream;
        (void)stream.Write(state.Data(), state.Size());
        (void)stream.Seek(0, SeekOrigin::Begin);
        Array<ReplicatedEntry> entries;
        {
            BinarySerializer reader(stream, SerializeMode::Read);
            foundation::core::Serialize(reader, "entries", entries);
        }
        for (const ReplicatedEntry& entry : entries)
        {
            if (entry.behavior >= component->behaviors.Size())
            {
                continue;
            }
            ScriptBehavior& behavior = component->behaviors[entry.behavior];
            const ScriptClass* scriptClass = behavior.script.Get();
            if (scriptClass == nullptr ||
                ScriptPropertyNameHash(scriptClass->className.AsView()) != entry.classHash)
            {
                continue; // not the behaviour the host meant
            }
            for (const ScriptPropertyDesc& property : scriptClass->properties)
            {
                if (property.hash != entry.property || !property.replicated)
                {
                    continue;
                }
                behavior.SetOverride(property.hash, entry.value); // for an instance made later
                if (behavior.instance.Get() != nullptr)
                {
                    String setter(property.name.AsView());
                    setter += u8"=";
                    Variant args[1] = {ScriptPropertyValueToVariant(entry.value, property.type, nullptr)};
                    (void)behavior.instance->Invoke(setter.AsView(), Span<Variant>{args, 1});
                }
            }
        }
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
        // send and emit: arity families with a Variant payload, as entity.send and scene.events.emit.
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
