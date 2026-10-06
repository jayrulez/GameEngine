// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell
// Editor::Scene - the scene editor's live EDITING tools (Sedulous fb32ee69).
//
// What an agent builds a level with on an open scene or prefab page: entity_create,
// entity_update, entity_delete, component_add, component_remove, prefab_spawn, behavior_add and
// behavior_set. Each call is one undo step through the page's command stack (its commands in a
// group, locked when it closes so the next call does not coalesce into it), the page dirty after
// and nothing saved, as component_set. An entity is named by guid, by name or by slash path;
// every result carries the entity as entity_inspect shows it. Refused while the page simulates,
// since Simulate's stop would discard the edit.
module;
#include "Core/Prelude.h"

module editor.scene;

import foundation.core;
import foundation.json;
import foundation.mcp;
import foundation.scene;
import foundation.scene.resource;
import foundation.resource;
import foundation.script.resource;
import engine.script;
import editor.core;

using namespace foundation::core;
using foundation::json::JsonValue;
using foundation::mcp::SchemaBuilder;
using foundation::mcp::ToolAnnotations;
using foundation::mcp::ToolResult;
namespace scene = foundation::scene;
namespace script = foundation::script;

namespace editor
{
    namespace
    {
        constexpr StringView kPage =
            u8"the scene or prefab page's asset guid (default: the active page)";
        constexpr StringView kEntity = u8"the entity: its guid, its name, or a slash path";

        /// A page the edit tools may change: a scene page that is not simulating.
        Result<AddressedPage, String> EditablePage(EditorContext& context, const JsonValue& args)
        {
            Result<AddressedPage, String> addressed = ResolveScenePage(context, args);
            if (addressed.HasValue() && addressed.Value().scene->IsSimulating())
            {
                return Err(Format(u8"page '{}' is simulating - edits are locked until "
                                  u8"simulate_stop",
                                  addressed.Value().page->Title()));
            }
            return addressed;
        }

        /// One call is one undo step: its commands in a group, the group locked when it closes
        /// so the next call's group (the same type) does not coalesce into it.
        struct Step
        {
            explicit Step(SceneEditContext& edit) : commands(&edit.Commands())
            {
                commands->BeginGroup(u8"mcp");
            }
            ~Step()
            {
                commands->EndGroup();
                commands->LockGroup();
            }
            Step(const Step&) = delete;
            Step& operator=(const Step&) = delete;
            EditorCommandStack* commands;
        };

        JsonValue Answer(EditorContext& context, const AddressedPage& addressed, const Guid& id)
        {
            SceneEditContext& edit = addressed.scene->EditContext();
            JsonValue out = JsonValue::MakeObject();
            out.Set(u8"page", PageJson(*addressed.page));
            out.Set(u8"entity", EntityJson(edit.Scene(), edit.Resolve(id), context.Resources()));
            return out;
        }

        /// The entity `key` names (required).
        Result<Guid, String> RequiredEntity(const AddressedPage& addressed, const JsonValue& args,
                                            StringView key)
        {
            const String text = args.Get(String(key)).AsString();
            if (text.IsEmpty())
            {
                return Err(Format(u8"`{}` names the entity (guid, name or path)", key));
            }
            return ResolveSceneEntity(addressed, text.AsView());
        }

        /// The entity `key` names, nil when absent or empty.
        Result<Guid, String> OptionalEntity(const AddressedPage& addressed, const JsonValue& args,
                                            StringView key)
        {
            const String text = args.Get(String(key)).AsString();
            if (text.IsEmpty())
            {
                return Guid{};
            }
            return ResolveSceneEntity(addressed, text.AsView());
        }

        Result<Guid, String> GuidArg(const JsonValue& args, StringView key)
        {
            const String text = args.Get(String(key)).AsString();
            Guid id;
            if (!Guid::TryParse(text.AsView(), id))
            {
                return Err(Format(u8"`{}` takes a guid; '{}' is not one", key, text.AsView()));
            }
            return id;
        }

        /// The transform arguments over `current`; `given` when any was.
        Result<Transform, String> ReadTransform(const JsonValue& args, Transform current,
                                                bool& given)
        {
            given = false;
            if (args.Has(u8"position"))
            {
                if (!ReadFloat3(args.Get(u8"position"), current.position))
                {
                    return Err(String(u8"`position` takes [x, y, z]"));
                }
                given = true;
            }
            if (args.Has(u8"rotation"))
            {
                const JsonValue rotation = args.Get(u8"rotation");
                if (!rotation.IsArray() || rotation.Count() != 4)
                {
                    return Err(String(u8"`rotation` takes a quaternion [x, y, z, w]"));
                }
                current.rotation = Quaternion{static_cast<f32>(rotation.At(0).AsNumber()),
                                              static_cast<f32>(rotation.At(1).AsNumber()),
                                              static_cast<f32>(rotation.At(2).AsNumber()),
                                              static_cast<f32>(rotation.At(3).AsNumber())};
                given = true;
            }
            else if (args.Has(u8"yawDegrees"))
            {
                const JsonValue yaw = args.Get(u8"yawDegrees");
                if (!yaw.IsNumber())
                {
                    return Err(String(u8"`yawDegrees` takes a number"));
                }
                current.rotation = Quaternion::FromAxisAngle(
                    Float3{0.0f, 1.0f, 0.0f}, DegreesToRadians(static_cast<f32>(yaw.AsNumber())));
                given = true;
            }
            if (args.Has(u8"scale"))
            {
                if (!ReadFloat3(args.Get(u8"scale"), current.scale))
                {
                    return Err(String(u8"`scale` takes [x, y, z]"));
                }
                given = true;
            }
            return current;
        }

        /// A manager for a component name, whether or not any entity has one: by wire name or by
        /// the component type's name.
        scene::ComponentManagerBase* ManagerByName(scene::Scene& scene, StringView name)
        {
            scene::ComponentManagerBase* found = nullptr;
            scene.ForEachManager(
                [&](scene::ComponentManagerBase& manager)
                {
                    const TypeInfo* type = manager.ComponentType();
                    if (found == nullptr &&
                        (manager.SerializationTypeId() == name ||
                         (type != nullptr &&
                          StringView(reinterpret_cast<const utf8char*>(type->name)) == name)))
                    {
                        found = &manager;
                    }
                });
            return found;
        }

        /// The cooked class, its properties the source of truth for a behaviour's.
        Result<script::ScriptClass*, String> CookedClass(EditorContext& context, const Guid& id)
        {
            script::ScriptClass* scriptClass =
                context.Resources() != nullptr
                    ? context.Resources()->Bind<script::ScriptClass>(id).Get()
                    : nullptr;
            if (scriptClass == nullptr || scriptClass->className.IsEmpty())
            {
                utf8char text[37];
                id.ToChars(text);
                return Err(Format(u8"script {} is not a cooked script class (script_create, then "
                                  u8"asset_cook)",
                                  StringView(text, 36)));
            }
            return scriptClass;
        }

        struct PropertyChange
        {
            u64 hash = 0;
            script::ScriptPropertyValue value;
        };

        /// Each named property's value, typed by the class's description of it; all checked
        /// before anything changes.
        Result<Array<PropertyChange>, String> ReadProperties(const AddressedPage& addressed,
                                                             const script::ScriptClass& scriptClass,
                                                             const JsonValue& args)
        {
            Array<PropertyChange> out;
            if (!args.Has(u8"properties"))
            {
                return out;
            }
            const JsonValue properties = args.Get(u8"properties");
            if (!properties.IsObject())
            {
                return Err(String(u8"`properties` takes an object: {\"speed\": 4}"));
            }
            for (i64 i = 0; i < properties.Count(); ++i)
            {
                const String name = properties.KeyAt(i);
                const JsonValue value = properties.Get(name);
                const script::ScriptPropertyDesc* desc =
                    scriptClass.FindProperty(script::ScriptPropertyNameHash(name.AsView()));
                if (desc == nullptr)
                {
                    String names;
                    for (const script::ScriptPropertyDesc& p : scriptClass.properties)
                    {
                        names += names.IsEmpty() ? u8"" : u8", ";
                        names += p.name;
                    }
                    return Err(Format(u8"{} has no property '{}'; its properties are: {}",
                                      scriptClass.className.AsView(), name.AsView(),
                                      names.IsEmpty() ? StringView(u8"none") : names.AsView()));
                }
                PropertyChange change;
                change.hash = desc->hash;
                change.value.kind = desc->type;
                bool fits = false;
                switch (desc->type)
                {
                case script::ScriptPropertyType::Float:
                case script::ScriptPropertyType::Int:
                    fits = value.IsNumber();
                    change.value.number = desc->type == script::ScriptPropertyType::Int
                                              ? static_cast<f64>(value.AsInt())
                                              : value.AsNumber();
                    break;
                case script::ScriptPropertyType::Bool:
                    fits = value.IsBool();
                    change.value.boolean = value.AsBool();
                    break;
                case script::ScriptPropertyType::String:
                    fits = value.IsString();
                    change.value.text = value.AsString();
                    break;
                case script::ScriptPropertyType::Vec3:
                    fits = ReadFloat3(value, change.value.vector);
                    break;
                case script::ScriptPropertyType::Color:
                    fits = value.IsArray() && value.Count() == 4;
                    if (fits)
                    {
                        change.value.color = Color{static_cast<f32>(value.At(0).AsNumber()),
                                                   static_cast<f32>(value.At(1).AsNumber()),
                                                   static_cast<f32>(value.At(2).AsNumber()),
                                                   static_cast<f32>(value.At(3).AsNumber())};
                    }
                    break;
                case script::ScriptPropertyType::Entity:
                    if (value.IsNull())
                    {
                        fits = true;
                    }
                    else if (value.IsString())
                    {
                        Result<Guid, String> target =
                            ResolveSceneEntity(addressed, value.AsString().AsView());
                        if (!target.HasValue())
                        {
                            return Err(Move(target.Error()));
                        }
                        change.value.guid = target.Value();
                        fits = true;
                    }
                    break;
                case script::ScriptPropertyType::Asset:
                    fits = value.IsNull() ||
                           (value.IsString() &&
                            Guid::TryParse(value.AsString().AsView(), change.value.guid));
                    break;
                case script::ScriptPropertyType::None:
                    break;
                }
                if (!fits)
                {
                    return Err(Format(u8"{}.{} is of type {}: the value does not fit (a number, "
                                      u8"true/false, a string, [x, y, z], [r, g, b, a], an entity "
                                      u8"or null, an asset guid or null)",
                                      scriptClass.className.AsView(), name.AsView(),
                                      script::ScriptPropertyTypeName(desc->type)));
                }
                out.PushBack(Move(change));
            }
            return out;
        }

        void TransformSchema(SchemaBuilder& schema)
        {
            schema.Arr(u8"position", u8"number", u8"the local position [x, y, z]")
                .Arr(u8"rotation", u8"number", u8"the local rotation as a quaternion [x, y, z, w]")
                .Number(u8"yawDegrees", u8"or the local rotation as a turn about +Y, in degrees")
                .Arr(u8"scale", u8"number", u8"the local scale [x, y, z]");
        }

        JsonValue PropertiesSchema()
        {
            JsonValue property = JsonValue::MakeObject();
            property.Set(u8"type", JsonValue::MakeString(u8"object"));
            property.Set(u8"description",
                         JsonValue::MakeString(
                             u8"the behaviour's properties by name: a number for a float or int, "
                             u8"true/false, a string, [x, y, z] for a vector, [r, g, b, a] for a "
                             u8"colour, an entity (guid, name or path) or null for an entity "
                             u8"reference, an asset guid or null for an asset reference"));
            // A map of names the tool resolves itself (it refuses one it does not know, with its
            // own list): the server's schema check takes any key here.
            property.Set(u8"additionalProperties", JsonValue::MakeBool(true));
            return property;
        }

        // ---- the tools ----

        ToolResult Create(EditorContext& context, const JsonValue& args)
        {
            Result<AddressedPage, String> addressed = EditablePage(context, args);
            if (!addressed.HasValue())
            {
                return Err(Move(addressed.Error()));
            }
            SceneEditContext& edit = addressed.Value().scene->EditContext();
            Result<Guid, String> parent = OptionalEntity(addressed.Value(), args, u8"parent");
            if (!parent.HasValue())
            {
                return Err(Move(parent.Error()));
            }
            bool given = false;
            Result<Transform, String> transform = ReadTransform(args, Transform{}, given);
            if (!transform.HasValue())
            {
                return Err(Move(transform.Error()));
            }
            const String name = args.Get(u8"name").AsString();
            Guid id;
            {
                Step step(edit);
                id = edit.CreateEntity(name.AsView(), parent.Value());
                if (!id.IsNil() && given)
                {
                    edit.SetLocalTransform(id, transform.Value());
                }
            }
            if (id.IsNil())
            {
                return Err(String(u8"the entity was not created (a parent that is gone?)"));
            }
            return Answer(context, addressed.Value(), id);
        }

        ToolResult Update(EditorContext& context, const JsonValue& args)
        {
            Result<AddressedPage, String> addressed = EditablePage(context, args);
            if (!addressed.HasValue())
            {
                return Err(Move(addressed.Error()));
            }
            SceneEditContext& edit = addressed.Value().scene->EditContext();
            Result<Guid, String> id = RequiredEntity(addressed.Value(), args, u8"entity");
            if (!id.HasValue())
            {
                return Err(Move(id.Error()));
            }
            const bool moving = args.Has(u8"parent");
            Result<Guid, String> parent = OptionalEntity(addressed.Value(), args, u8"parent");
            if (!parent.HasValue())
            {
                return Err(Move(parent.Error()));
            }
            if (moving && !parent.Value().IsNil() &&
                edit.IsSelfOrAncestor(parent.Value(), id.Value()))
            {
                return Err(String(u8"an entity cannot move under itself or its own descendant"));
            }
            bool given = false;
            Result<Transform, String> transform = ReadTransform(
                args, edit.Scene().GetLocalTransform(edit.Resolve(id.Value())), given);
            if (!transform.HasValue())
            {
                return Err(Move(transform.Error()));
            }
            {
                Step step(edit);
                if (args.Has(u8"name"))
                {
                    edit.RenameEntity(id.Value(), args.Get(u8"name").AsString().AsView());
                }
                if (moving)
                {
                    edit.ReparentEntity(id.Value(), parent.Value());
                }
                if (args.Has(u8"active"))
                {
                    edit.SetEntityActive(id.Value(), args.Get(u8"active").AsBool());
                }
                if (given)
                {
                    edit.SetLocalTransform(id.Value(), transform.Value());
                }
            }
            return Answer(context, addressed.Value(), id.Value());
        }

        ToolResult Delete(EditorContext& context, const JsonValue& args)
        {
            Result<AddressedPage, String> addressed = EditablePage(context, args);
            if (!addressed.HasValue())
            {
                return Err(Move(addressed.Error()));
            }
            SceneEditContext& edit = addressed.Value().scene->EditContext();
            Result<Guid, String> id = RequiredEntity(addressed.Value(), args, u8"entity");
            if (!id.HasValue())
            {
                return Err(Move(id.Error()));
            }
            {
                Step step(edit);
                edit.DestroyEntity(id.Value());
            }
            JsonValue out = JsonValue::MakeObject();
            out.Set(u8"page", PageJson(*addressed.Value().page));
            out.Set(u8"deleted", GuidJson(id.Value()));
            return out;
        }

        ToolResult AddComponent(EditorContext& context, const JsonValue& args)
        {
            Result<AddressedPage, String> addressed = EditablePage(context, args);
            if (!addressed.HasValue())
            {
                return Err(Move(addressed.Error()));
            }
            SceneEditContext& edit = addressed.Value().scene->EditContext();
            Result<Guid, String> id = RequiredEntity(addressed.Value(), args, u8"entity");
            if (!id.HasValue())
            {
                return Err(Move(id.Error()));
            }
            const String name = args.Get(u8"component").AsString();
            scene::ComponentManagerBase* manager = ManagerByName(edit.Scene(), name.AsView());
            if (manager == nullptr || manager->ComponentType() == nullptr)
            {
                return Err(Format(u8"no component '{}' in this build (component_schema lists them "
                                  u8"by wire name)",
                                  name.AsView()));
            }
            const scene::EntityHandle handle = edit.Resolve(id.Value());
            if (manager->HasComponent(handle))
            {
                return Err(Format(u8"'{}' already has a {}", edit.Scene().GetEntityName(handle),
                                  name.AsView()));
            }
            {
                Step step(edit);
                edit.AddComponent(id.Value(), manager->ComponentType());
            }
            return Answer(context, addressed.Value(), id.Value());
        }

        ToolResult RemoveComponent(EditorContext& context, const JsonValue& args)
        {
            Result<AddressedPage, String> addressed = EditablePage(context, args);
            if (!addressed.HasValue())
            {
                return Err(Move(addressed.Error()));
            }
            SceneEditContext& edit = addressed.Value().scene->EditContext();
            Result<Guid, String> id = RequiredEntity(addressed.Value(), args, u8"entity");
            if (!id.HasValue())
            {
                return Err(Move(id.Error()));
            }
            const String name = args.Get(u8"component").AsString();
            const scene::EntityHandle handle = edit.Resolve(id.Value());
            scene::ComponentManagerBase* manager =
                FindComponentManager(edit.Scene(), handle, name.AsView());
            if (manager == nullptr || manager->ComponentType() == nullptr)
            {
                return Err(Format(u8"'{}' has no component '{}' (entity_inspect lists its "
                                  u8"components)",
                                  edit.Scene().GetEntityName(handle), name.AsView()));
            }
            {
                Step step(edit);
                edit.RemoveComponent(id.Value(), manager->ComponentType());
            }
            return Answer(context, addressed.Value(), id.Value());
        }

        ToolResult SpawnPrefab(EditorContext& context, const JsonValue& args)
        {
            Result<AddressedPage, String> addressed = EditablePage(context, args);
            if (!addressed.HasValue())
            {
                return Err(Move(addressed.Error()));
            }
            SceneEditContext& edit = addressed.Value().scene->EditContext();
            Result<Guid, String> prefab = GuidArg(args, u8"prefab");
            if (!prefab.HasValue())
            {
                return Err(Move(prefab.Error()));
            }
            if (prefab.Value() == addressed.Value().page->InstanceId())
            {
                return Err(String(u8"a prefab cannot contain an instance of itself"));
            }
            Result<Guid, String> parent = OptionalEntity(addressed.Value(), args, u8"parent");
            if (!parent.HasValue())
            {
                return Err(Move(parent.Error()));
            }
            bool given = false;
            Result<Transform, String> transform = ReadTransform(args, Transform{}, given);
            if (!transform.HasValue())
            {
                return Err(Move(transform.Error()));
            }
            const scene::PrefabPayloadResolver* resolver = edit.PrefabResolver();
            UniquePtr<IStream> payload =
                (resolver != nullptr && *resolver) ? (*resolver)(prefab.Value()) : UniquePtr<IStream>{};
            if (payload.Get() == nullptr)
            {
                utf8char text[37];
                prefab.Value().ToChars(text);
                return Err(Format(u8"no prefab {} with content in the project (asset_list: type "
                                  u8"PrefabDocument; a new prefab has content once written)",
                                  StringView(text, 36)));
            }
            Array<byte> bytes;
            bytes.Resize(static_cast<usize>(payload->Size()));
            (void)payload->Read(bytes.Data(), bytes.Size());
            Guid root;
            {
                Step step(edit);
                root = edit.SpawnPrefabInstance(prefab.Value(), Move(bytes), parent.Value(),
                                                given ? &transform.Value() : nullptr);
            }
            if (root.IsNil())
            {
                utf8char text[37];
                prefab.Value().ToChars(text);
                return Err(Format(u8"prefab {} did not spawn (log_read, category Prefab, says why)",
                                  StringView(text, 36)));
            }
            return Answer(context, addressed.Value(), root);
        }

        ToolResult AddBehavior(EditorContext& context, const JsonValue& args)
        {
            Result<AddressedPage, String> addressed = EditablePage(context, args);
            if (!addressed.HasValue())
            {
                return Err(Move(addressed.Error()));
            }
            SceneEditContext& edit = addressed.Value().scene->EditContext();
            Result<Guid, String> id = RequiredEntity(addressed.Value(), args, u8"entity");
            if (!id.HasValue())
            {
                return Err(Move(id.Error()));
            }
            Result<Guid, String> scriptId = GuidArg(args, u8"script");
            if (!scriptId.HasValue())
            {
                return Err(Move(scriptId.Error()));
            }
            Result<script::ScriptClass*, String> scriptClass = CookedClass(context, scriptId.Value());
            if (!scriptClass.HasValue())
            {
                return Err(Move(scriptClass.Error()));
            }
            Result<Array<PropertyChange>, String> changes =
                ReadProperties(addressed.Value(), *scriptClass.Value(), args);
            if (!changes.HasValue())
            {
                return Err(Move(changes.Error()));
            }
            const TypeInfo* scriptType = &TypeOf<engine::script::ScriptComponent>();
            scene::ComponentManagerBase* scripts = edit.FindManager(scriptType);
            if (scripts == nullptr)
            {
                return Err(String(u8"this scene has no script components"));
            }
            i64 index = -1;
            {
                Step step(edit);
                if (!scripts->HasComponent(edit.Resolve(id.Value())))
                {
                    edit.AddComponent(id.Value(), scriptType);
                }
                const Guid script = scriptId.Value();
                const Array<PropertyChange>& values = changes.Value();
                (void)edit.MutateComponent<engine::script::ScriptComponent>(
                    id.Value(),
                    [&](engine::script::ScriptComponent& component)
                    {
                        engine::script::ScriptBehavior behavior;
                        behavior.script.SetId(script);
                        for (const PropertyChange& change : values)
                        {
                            behavior.SetOverride(change.hash, change.value);
                        }
                        component.behaviors.PushBack(Move(behavior));
                        index = static_cast<i64>(component.behaviors.Size()) - 1;
                    });
            }
            if (index < 0)
            {
                return Err(String(u8"the behaviour was not added (the Script component would not "
                                  u8"take it)"));
            }
            JsonValue out = Answer(context, addressed.Value(), id.Value());
            out.Set(u8"index", JsonValue::MakeNumber(static_cast<f64>(index)));
            return out;
        }

        ToolResult SetBehavior(EditorContext& context, const JsonValue& args)
        {
            Result<AddressedPage, String> addressed = EditablePage(context, args);
            if (!addressed.HasValue())
            {
                return Err(Move(addressed.Error()));
            }
            SceneEditContext& edit = addressed.Value().scene->EditContext();
            Result<Guid, String> id = RequiredEntity(addressed.Value(), args, u8"entity");
            if (!id.HasValue())
            {
                return Err(Move(id.Error()));
            }
            const scene::EntityHandle handle = edit.Resolve(id.Value());
            scene::ComponentManagerBase* scripts =
                edit.FindManager(&TypeOf<engine::script::ScriptComponent>());
            const auto* component =
                (scripts != nullptr && scripts->HasComponent(handle))
                    ? static_cast<const engine::script::ScriptComponent*>(
                          scripts->GetComponentInstance(handle).Pointer())
                    : nullptr;
            const i64 index = args.Get(u8"index").AsInt(0);
            if (component == nullptr || index < 0 ||
                index >= static_cast<i64>(component->behaviors.Size()))
            {
                return Err(Format(u8"'{}' has no behaviour {} (entity_inspect lists its Script "
                                  u8"component's)",
                                  edit.Scene().GetEntityName(handle), index));
            }
            Result<script::ScriptClass*, String> scriptClass = CookedClass(
                context, component->behaviors[static_cast<usize>(index)].script.id);
            if (!scriptClass.HasValue())
            {
                return Err(Move(scriptClass.Error()));
            }
            Result<Array<PropertyChange>, String> changes =
                ReadProperties(addressed.Value(), *scriptClass.Value(), args);
            if (!changes.HasValue())
            {
                return Err(Move(changes.Error()));
            }
            const bool setEnabled = args.Has(u8"enabled");
            const bool enabled = args.Get(u8"enabled").AsBool();
            {
                Step step(edit);
                const Array<PropertyChange>& values = changes.Value();
                (void)edit.MutateComponent<engine::script::ScriptComponent>(
                    id.Value(),
                    [&](engine::script::ScriptComponent& live)
                    {
                        if (static_cast<usize>(index) >= live.behaviors.Size())
                        {
                            return;
                        }
                        engine::script::ScriptBehavior& behavior =
                            live.behaviors[static_cast<usize>(index)];
                        for (const PropertyChange& change : values)
                        {
                            behavior.SetOverride(change.hash, change.value);
                        }
                        if (setEnabled)
                        {
                            behavior.enabled = enabled;
                        }
                    });
            }
            return Answer(context, addressed.Value(), id.Value());
        }
    }

    void RegisterSceneEditTools(foundation::mcp::McpServer& server, EditorContext& context)
    {
        EditorContext* ctx = &context;
        {
            SchemaBuilder schema;
            schema.Str(u8"page", kPage)
                .Str(u8"name", u8"the new entity's name", true)
                .Str(u8"parent", u8"the entity to create it under (default: a scene root)");
            TransformSchema(schema);
            server.RegisterTool(
                u8"entity_create",
                u8"Create an empty entity on a scene page, under `parent` or at the root, placed by "
                u8"`position`, `rotation` ([x, y, z, w]) or `yawDegrees`, and `scale`: one undo "
                u8"step. Add components with component_add and behaviours with behavior_add; place "
                u8"a prefab with prefab_spawn instead. Returns the entity as entity_inspect shows "
                u8"it.",
                schema.Build(), ToolAnnotations::Creates(),
                [ctx](const JsonValue& args) -> ToolResult { return Create(*ctx, args); });
        }
        {
            SchemaBuilder schema;
            schema.Str(u8"page", kPage)
                .Str(u8"entity", kEntity, true)
                .Str(u8"name", u8"a new name")
                .Str(u8"parent", u8"a new parent (\"\" moves it to the root); its local "
                                 u8"transform is kept")
                .Boolean(u8"active", u8"switch it on or off");
            TransformSchema(schema);
            server.RegisterTool(
                u8"entity_update",
                u8"Change an entity on a scene page: its name, parent, active flag and local "
                u8"transform (`position`, `rotation` [x, y, z, w] or `yawDegrees`, `scale`; what "
                u8"is given, the rest kept). All of it one undo step. Returns the entity as "
                u8"entity_inspect shows it.",
                schema.Build(), ToolAnnotations::Adjusts(),
                [ctx](const JsonValue& args) -> ToolResult { return Update(*ctx, args); });
        }
        server.RegisterTool(
            u8"entity_delete",
            u8"Delete an entity and everything under it from a scene page: one undo step. Returns "
            u8"the deleted entity's guid.",
            SchemaBuilder().Str(u8"page", kPage).Str(u8"entity", kEntity, true).Build(),
            ToolAnnotations::Overwrites(),
            [ctx](const JsonValue& args) -> ToolResult { return Delete(*ctx, args); });
        server.RegisterTool(
            u8"component_add",
            u8"Add a component, at its defaults, to an entity on a scene page: one undo step; "
            u8"component_set then writes its fields. Refused when the entity already has one. "
            u8"Returns the entity as entity_inspect shows it.",
            SchemaBuilder()
                .Str(u8"page", kPage)
                .Str(u8"entity", kEntity, true)
                .Str(u8"component", u8"the component's wire name (\"physics.RigidBody\", "
                                    u8"\"light\"; component_schema lists them) or type name",
                     true)
                .Build(),
            ToolAnnotations::Creates(),
            [ctx](const JsonValue& args) -> ToolResult { return AddComponent(*ctx, args); });
        server.RegisterTool(
            u8"component_remove",
            u8"Remove a component from an entity on a scene page: one undo step. Returns the "
            u8"entity as entity_inspect shows it.",
            SchemaBuilder()
                .Str(u8"page", kPage)
                .Str(u8"entity", kEntity, true)
                .Str(u8"component", u8"the component, as entity_inspect names it", true)
                .Build(),
            ToolAnnotations::Overwrites(),
            [ctx](const JsonValue& args) -> ToolResult { return RemoveComponent(*ctx, args); });
        {
            SchemaBuilder schema;
            schema.Str(u8"page", kPage)
                .Str(u8"prefab", u8"the prefab's guid (asset_list: type PrefabDocument; an "
                                 u8"imported model's is the `Prefab` beside its manifest)",
                     true)
                .Str(u8"parent", u8"the entity to place it under (default: a scene root)");
            TransformSchema(schema);
            server.RegisterTool(
                u8"prefab_spawn",
                u8"Place an instance of a prefab on a scene page, under `parent` or at the root, "
                u8"its root at `position`, `rotation` ([x, y, z, w]) or `yawDegrees`, and `scale`: "
                u8"one undo step, the instance linked to its prefab as the editor's own spawn links "
                u8"it. Returns the instance's root as entity_inspect shows it.",
                schema.Build(), ToolAnnotations::Creates(),
                [ctx](const JsonValue& args) -> ToolResult { return SpawnPrefab(*ctx, args); });
        }
        server.RegisterTool(
            u8"behavior_add",
            u8"Attach a script behaviour to an entity on a scene page, adding its Script component "
            u8"when it has none, with `properties` set over the class's defaults: one undo step. "
            u8"The class must be cooked (asset_cook): its properties and their types come from "
            u8"the cooked class. Returns the entity as entity_inspect shows it and the "
            u8"behaviour's index.",
            SchemaBuilder()
                .Str(u8"page", kPage)
                .Str(u8"entity", kEntity, true)
                .Str(u8"script", u8"the script class asset's guid (asset_list: type "
                                 u8"ScriptClassAsset); it must be cooked",
                     true)
                .Property(u8"properties", PropertiesSchema())
                .Build(),
            ToolAnnotations::Creates(),
            [ctx](const JsonValue& args) -> ToolResult { return AddBehavior(*ctx, args); });
        server.RegisterTool(
            u8"behavior_set",
            u8"Set properties of a script behaviour on an entity of a scene page (its `index`, "
            u8"default the first), and its enabled flag: one undo step. `properties` as "
            u8"behavior_add takes them. Returns the entity as entity_inspect shows it.",
            SchemaBuilder()
                .Str(u8"page", kPage)
                .Str(u8"entity", kEntity, true)
                .Integer(u8"index", u8"which of the entity's behaviours (default 0, the first)")
                .Property(u8"properties", PropertiesSchema())
                .Boolean(u8"enabled", u8"switch the behaviour on or off")
                .Build(),
            ToolAnnotations::Adjusts(),
            [ctx](const JsonValue& args) -> ToolResult { return SetBehavior(*ctx, args); });
    }
}
