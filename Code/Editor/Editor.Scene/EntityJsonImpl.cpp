// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell
// Editor::Scene - :entity_json partition (implementation).
module;
#include "Core/Prelude.h"

module editor.scene;

import foundation.core;
import foundation.json;
import foundation.scene;
import foundation.scene.resource;
import foundation.resource;
import foundation.script.resource; // ScriptClass, ScriptPropertyValue (a behaviour's overrides)
import engine.script;              // ScriptComponent (its behaviours, a hidden list)

using namespace foundation::core;
using foundation::json::JsonValue;
namespace scene = foundation::scene;

namespace editor
{
    JsonValue GuidJson(const Guid& id)
    {
        utf8char text[37];
        id.ToChars(text);
        return JsonValue::MakeString(StringView(text, 36));
    }

    JsonValue Float3Json(const Float3& v)
    {
        JsonValue out = JsonValue::MakeArray();
        out.Add(JsonValue::MakeNumber(v.x));
        out.Add(JsonValue::MakeNumber(v.y));
        out.Add(JsonValue::MakeNumber(v.z));
        return out;
    }
    JsonValue QuadJson(f32 a, f32 b, f32 c, f32 d)
    {
        JsonValue out = JsonValue::MakeArray();
        out.Add(JsonValue::MakeNumber(a));
        out.Add(JsonValue::MakeNumber(b));
        out.Add(JsonValue::MakeNumber(c));
        out.Add(JsonValue::MakeNumber(d));
        return out;
    }

    /// A leaf value as JSON: what the Variant's type says it is. A reference-shaped value
    /// (a resource ref) is its id, an enum its enumerator's name, an entity ref its guid.
    JsonValue ValueJson(const Variant& value)
    {
        if (value.IsEmpty() || value.Type() == nullptr)
        {
            return JsonValue::MakeNull();
        }
        const TypeInfo& type = *value.Type();
        if (IsReferenceType(type))
        {
            const Guid* id = type.reference->Id(value.ValuePointer());
            return (id != nullptr && !id->IsNil()) ? GuidJson(*id) : JsonValue::MakeNull();
        }
        if (type.enumeratorCount > 0)
        {
            const i64 raw = value.AsEnumInt();
            for (u32 i = 0; i < type.enumeratorCount; ++i)
            {
                if (type.enumerators[i].value == raw)
                {
                    return JsonValue::MakeString(StringView(
                        reinterpret_cast<const utf8char*>(type.enumerators[i].name)));
                }
            }
            return JsonValue::MakeNumber(static_cast<f64>(raw));
        }
        if (value.Is<bool>())
        {
            return JsonValue::MakeBool(value.Get<bool>());
        }
        if (value.Is<f32>())
        {
            return JsonValue::MakeNumber(value.Get<f32>());
        }
        if (value.Is<f64>())
        {
            return JsonValue::MakeNumber(value.Get<f64>());
        }
        if (value.Is<i32>())
        {
            return JsonValue::MakeNumber(value.Get<i32>());
        }
        if (value.Is<u32>())
        {
            return JsonValue::MakeNumber(value.Get<u32>());
        }
        if (value.Is<i64>())
        {
            return JsonValue::MakeNumber(static_cast<f64>(value.Get<i64>()));
        }
        if (value.Is<u64>())
        {
            return JsonValue::MakeNumber(static_cast<f64>(value.Get<u64>()));
        }
        if (value.Is<i16>())
        {
            return JsonValue::MakeNumber(value.Get<i16>());
        }
        if (value.Is<u16>())
        {
            return JsonValue::MakeNumber(value.Get<u16>());
        }
        if (value.Is<i8>())
        {
            return JsonValue::MakeNumber(value.Get<i8>());
        }
        if (value.Is<u8>())
        {
            return JsonValue::MakeNumber(value.Get<u8>());
        }
        if (value.Is<String>())
        {
            return JsonValue::MakeString(value.Get<String>());
        }
        if (value.Is<Guid>())
        {
            return GuidJson(value.Get<Guid>());
        }
        if (value.Is<scene::EntityRef>())
        {
            const Guid& id = value.Get<scene::EntityRef>().id;
            return id.IsNil() ? JsonValue::MakeNull() : GuidJson(id);
        }
        if (value.Is<Float2>())
        {
            const Float2& v = value.Get<Float2>();
            JsonValue out = JsonValue::MakeArray();
            out.Add(JsonValue::MakeNumber(v.x));
            out.Add(JsonValue::MakeNumber(v.y));
            return out;
        }
        if (value.Is<Float3>())
        {
            return Float3Json(value.Get<Float3>());
        }
        if (value.Is<Float4>())
        {
            const Float4& v = value.Get<Float4>();
            return QuadJson(v.x, v.y, v.z, v.w);
        }
        if (value.Is<Quaternion>())
        {
            const Quaternion& q = value.Get<Quaternion>();
            return QuadJson(q.x, q.y, q.z, q.w);
        }
        if (value.Is<Color>())
        {
            const Color& c = value.Get<Color>();
            return QuadJson(c.r, c.g, c.b, c.a);
        }
        // A value type this reader has no spelling for: its type name, so the agent knows
        // there is something here it cannot read yet.
        JsonValue unknown = JsonValue::MakeObject();
        unknown.Set(u8"unreadable", JsonValue::MakeString(StringView(
                                        reinterpret_cast<const utf8char*>(type.name))));
        return unknown;
    }

    /// One property: a leaf through its Variant; a nested structure recursed into; a
    /// container as an array of its elements (each a leaf or a structure).
    JsonValue PropertyJson(const PropertyInfo& property, const Instance& instance)
    {
        // A list reads as its elements whether or not it is flagged nested (an animator's list of
        // entity references is a plain container property).
        const bool list = property.type != nullptr && property.type->container != nullptr;
        if (!IsNested(property) && !list)
        {
            return ValueJson(GetProperty(property, instance));
        }
        void* address = property.address != nullptr ? property.address(instance) : nullptr;
        if (address == nullptr || property.type == nullptr)
        {
            return JsonValue::MakeNull();
        }
        const Instance nested(address, property.type);
        if (property.type->container != nullptr)
        {
            const ContainerInfo& container = *property.type->container;
            JsonValue items = JsonValue::MakeArray();
            const usize count = ContainerSize(container, nested);
            for (usize i = 0; i < count; ++i)
            {
                const Variant element = ContainerGetAt(container, nested, i);
                if (!element.IsEmpty() && element.Type() != nullptr &&
                    element.Type()->propertyCount > 0 && !IsReferenceType(*element.Type()))
                {
                    items.Add(PropertiesJson(
                        *element.Type(),
                        Instance(const_cast<void*>(element.ValuePointer()), element.Type())));
                }
                else
                {
                    items.Add(ValueJson(element));
                }
            }
            return items;
        }
        return PropertiesJson(*property.type, nested);
    }

    /// A reflected structure's properties, by name, in declaration order.
    JsonValue PropertiesJson(const TypeInfo& type, const Instance& instance)
    {
        JsonValue out = JsonValue::MakeObject();
        for (const PropertyInfo& property : Properties(type))
        {
            out.Set(StringView(reinterpret_cast<const utf8char*>(property.name)),
                    PropertyJson(property, instance));
        }
        return out;
    }

    /// The entity as the agent sees it: identity, hierarchy, transform, and every
    /// component the scene holds for it with its reflected properties.
    namespace
    {
        JsonValue ScriptValueJson(const foundation::script::ScriptPropertyValue& value)
        {
            using foundation::script::ScriptPropertyType;
            switch (value.kind)
            {
            case ScriptPropertyType::Float:
            case ScriptPropertyType::Int:
                return JsonValue::MakeNumber(value.number);
            case ScriptPropertyType::Bool:
                return JsonValue::MakeBool(value.boolean);
            case ScriptPropertyType::String:
                return JsonValue::MakeString(value.text);
            case ScriptPropertyType::Vec3:
                return Float3Json(value.vector);
            case ScriptPropertyType::Color:
                return QuadJson(value.color.r, value.color.g, value.color.b, value.color.a);
            case ScriptPropertyType::Entity:
            case ScriptPropertyType::Asset:
                return value.guid.IsNil() ? JsonValue::MakeNull() : GuidJson(value.guid);
            case ScriptPropertyType::None:
                break;
            }
            return JsonValue::MakeNull();
        }

        /// A Script component's behaviours: each one's script (its guid, and its class when the
        /// class is cooked), whether it runs, and its property overrides by name.
        JsonValue BehaviorsJson(const engine::script::ScriptComponent& component,
                                foundation::resource::ResourceManager* resources)
        {
            JsonValue list = JsonValue::MakeArray();
            for (const engine::script::ScriptBehavior& behavior : component.behaviors)
            {
                JsonValue entry = JsonValue::MakeObject();
                const Guid id = behavior.script.id;
                entry.Set(u8"script", id.IsNil() ? JsonValue::MakeNull() : GuidJson(id));
                const foundation::script::ScriptClass* scriptClass = behavior.script.Get();
                if (scriptClass == nullptr && resources != nullptr && !id.IsNil())
                {
                    scriptClass = resources->Bind<foundation::script::ScriptClass>(id).Get();
                }
                entry.Set(u8"class", scriptClass != nullptr
                                         ? JsonValue::MakeString(scriptClass->className)
                                         : JsonValue::MakeNull());
                entry.Set(u8"enabled", JsonValue::MakeBool(behavior.enabled));
                JsonValue properties = JsonValue::MakeObject();
                for (const engine::script::ScriptPropertyOverride& o : behavior.overrides)
                {
                    const foundation::script::ScriptPropertyDesc* desc =
                        scriptClass != nullptr ? scriptClass->FindProperty(o.nameHash) : nullptr;
                    properties.Set(desc != nullptr ? desc->name : Format(u8"#{}", o.nameHash),
                                   ScriptValueJson(o.value));
                }
                entry.Set(u8"properties", Move(properties));
                // Running (a PIE game, a Simulate): the instance's current values of the class's
                // properties, where `properties` holds what is authored.
                if (behavior.instance.Get() != nullptr && scriptClass != nullptr)
                {
                    JsonValue live = JsonValue::MakeObject();
                    for (const foundation::script::ScriptPropertyDesc& desc : scriptClass->properties)
                    {
                        Result<Variant> value = behavior.instance->GetProperty(desc.name.AsView());
                        if (value.HasValue())
                        {
                            live.Set(desc.name, ValueJson(value.Value()));
                        }
                    }
                    entry.Set(u8"live", Move(live));
                }
                list.Add(Move(entry));
            }
            return list;
        }
    }

    bool BehaviorField(scene::Scene& scene, scene::EntityHandle handle, StringView className,
                       StringView field, JsonValue& out)
    {
        auto* scripts = scene.GetSystem<engine::script::ScriptComponentManager>();
        const engine::script::ScriptComponent* component =
            scripts != nullptr ? scripts->Get(handle) : nullptr;
        if (component == nullptr)
        {
            return false;
        }
        for (const engine::script::ScriptBehavior& behavior : component->behaviors)
        {
            if (behavior.instance.Get() == nullptr || behavior.boundClass == nullptr ||
                behavior.boundClass->className.AsView() != className)
            {
                continue;
            }
            Result<Variant> value = behavior.instance->GetProperty(field);
            if (!value.HasValue())
            {
                return false;
            }
            out = ValueJson(value.Value());
            return true;
        }
        return false;
    }

    JsonValue EntityJson(scene::Scene& scene, scene::EntityHandle handle,
                         foundation::resource::ResourceManager* resources)
    {
        JsonValue out = JsonValue::MakeObject();
        out.Set(u8"guid", GuidJson(scene.GetEntityId(handle)));
        out.Set(u8"name", JsonValue::MakeString(String(scene.GetEntityName(handle))));
        out.Set(u8"active", JsonValue::MakeBool(scene.IsActive(handle)));
        const scene::EntityHandle parent = scene.GetParent(handle);
        out.Set(u8"parent", parent.IsAssigned() ? GuidJson(scene.GetEntityId(parent))
                                                : JsonValue::MakeNull());
        JsonValue children = JsonValue::MakeArray();
        for (scene::EntityHandle child = scene.GetFirstChild(handle); child.IsAssigned();
             child = scene.GetNextSibling(child))
        {
            children.Add(GuidJson(scene.GetEntityId(child)));
        }
        out.Set(u8"children", Move(children));
        const Transform local = scene.GetLocalTransform(handle);
        JsonValue transform = JsonValue::MakeObject();
        transform.Set(u8"position", Float3Json(local.position));
        transform.Set(u8"rotation", QuadJson(local.rotation.x, local.rotation.y,
                                             local.rotation.z, local.rotation.w));
        transform.Set(u8"scale", Float3Json(local.scale));
        out.Set(u8"transform", Move(transform));
        JsonValue components = JsonValue::MakeArray();
        scene.ForEachManager(
            [&](scene::ComponentManagerBase& manager)
            {
                if (!manager.HasComponent(handle))
                {
                    return;
                }
                JsonValue component = JsonValue::MakeObject();
                component.Set(u8"type",
                              JsonValue::MakeString(String(manager.SerializationTypeId())));
                const TypeInfo* type = manager.ComponentType();
                const Instance instance = manager.GetComponentInstance(handle);
                if (type != nullptr && instance.Pointer() != nullptr)
                {
                    component.Set(u8"typeName",
                                  JsonValue::MakeString(StringView(
                                      reinterpret_cast<const utf8char*>(type->name))));
                    component.Set(u8"properties", PropertiesJson(*type, instance));
                }
                else
                {
                    component.Set(u8"properties", JsonValue::MakeNull()); // unreflected
                }
                // The behaviours are a hidden list the reflected properties leave out.
                if (type == &TypeOf<engine::script::ScriptComponent>() &&
                    instance.Pointer() != nullptr)
                {
                    component.Set(u8"behaviors",
                                  BehaviorsJson(*static_cast<const engine::script::ScriptComponent*>(
                                                    instance.Pointer()),
                                                resources));
                }
                components.Add(Move(component));
            });
        out.Set(u8"components", Move(components));
        return out;
    }

    /// The manager holding `component` for the entity: by serialization id ("light",
    /// "physics.RigidBody") or by the reflected type's name ("LightComponent").
    scene::ComponentManagerBase* FindComponentManager(scene::Scene& scene,
                                                      scene::EntityHandle entity,
                                                      StringView component)
    {
        scene::ComponentManagerBase* found = nullptr;
        scene.ForEachManager(
            [&](scene::ComponentManagerBase& manager)
            {
                if (found != nullptr || !manager.HasComponent(entity))
                {
                    return;
                }
                const TypeInfo* type = manager.ComponentType();
                if (manager.SerializationTypeId() == component ||
                    (type != nullptr &&
                     StringView(reinterpret_cast<const utf8char*>(type->name)) == component))
                {
                    found = &manager;
                }
            });
        return found;
    }

    scene::EntityHandle FindEntity(scene::Scene& scene, StringView text)
    {
        Guid id;
        if (Guid::TryParse(text, id))
        {
            return scene.FindEntity(id);
        }
        bool isPath = false;
        for (const utf8char c : text)
        {
            isPath = isPath || c == u8'/';
        }
        if (isPath)
        {
            return scene.FindEntityByPath(text);
        }
        return scene.FindEntityByName(text);
    }

    Result<JsonValue, String> EntityFieldJson(scene::Scene& scene, scene::EntityHandle handle,
                                              StringView entityText, StringView path)
    {
        Array<StringView> segments;
        usize start = 0;
        for (usize i = 0; i <= path.Size(); ++i)
        {
            if (i == path.Size() || path[i] == u8'.')
            {
                segments.PushBack(path.SubStr(start, i - start));
                start = i + 1;
            }
        }
        JsonValue node;
        bool found = true;
        usize rest = 1;
        const StringView head = segments[0];
        if (head == u8"worldPosition")
        {
            node = Float3Json(scene.GetWorldPosition(handle));
        }
        else if (head == u8"position")
        {
            node = Float3Json(scene.GetLocalTransform(handle).position);
        }
        else if (head == u8"rotation")
        {
            const Quaternion q = scene.GetLocalTransform(handle).rotation;
            node = QuadJson(q.x, q.y, q.z, q.w);
        }
        else if (head == u8"scale")
        {
            node = Float3Json(scene.GetLocalTransform(handle).scale);
        }
        else if (head == u8"active")
        {
            node = JsonValue::MakeBool(scene.IsActive(handle));
        }
        else if (segments.Size() > 1 &&
                 BehaviorField(scene, handle, segments[0], segments[1], node))
        {
            rest = 2; // a running behaviour's field, `<BehaviorClass>.<field>`
        }
        else
        {
            // `<component>.<property>`: the component's name may hold dots
            // ("physics.RigidBody"), so try each split, the shortest component first.
            found = false;
            for (usize k = 1; k < segments.Size() && !found; ++k)
            {
                const StringView component =
                    path.SubStr(0, static_cast<usize>(segments[k].Data() - path.Data()) - 1);
                scene::ComponentManagerBase* manager =
                    FindComponentManager(scene, handle, component);
                const TypeInfo* type = manager != nullptr ? manager->ComponentType() : nullptr;
                if (type == nullptr)
                {
                    continue;
                }
                const Instance instance = manager->GetComponentInstance(handle);
                for (const PropertyInfo& property : Properties(*type))
                {
                    if (StringView(reinterpret_cast<const utf8char*>(property.name)) == segments[k])
                    {
                        node = PropertyJson(property, instance);
                        rest = k + 1;
                        found = true;
                        break;
                    }
                }
            }
        }
        if (!found)
        {
            return Err(Format(u8"entity '{}' has no field '{}' (worldPosition, position, rotation, "
                              u8"scale, active, <component>.<property> as entity_inspect names "
                              u8"them, or <BehaviorClass>.<field> of a behaviour running on it)",
                              entityText, path));
        }
        for (usize s = rest; s < segments.Size(); ++s)
        {
            const StringView segment = segments[s];
            JsonValue next;
            bool has = false;
            if (node.IsObject())
            {
                has = node.Has(String(segment));
                next = node.Get(String(segment));
            }
            else if (node.IsArray())
            {
                i64 index = -1;
                if (segment == u8"x")
                {
                    index = 0;
                }
                else if (segment == u8"y")
                {
                    index = 1;
                }
                else if (segment == u8"z")
                {
                    index = 2;
                }
                else if (segment == u8"w")
                {
                    index = 3;
                }
                else
                {
                    if (const Optional<i64> parsed = ParseInt(segment); parsed.HasValue())
                    {
                        index = *parsed;
                    }
                }
                has = index >= 0 && index < node.Count();
                if (has)
                {
                    next = node.At(index);
                }
            }
            if (!has)
            {
                return Err(Format(u8"'{}' of entity '{}': nothing at '{}'", path, entityText,
                                  segment));
            }
            node = Move(next);
        }
        return node;
    }
}
