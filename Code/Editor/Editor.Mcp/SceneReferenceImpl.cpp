// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell
// Editor::Mcp - the scene format reference generator (:scene_reference), the live docs://
// resources and the component_schema tool. See the partition's header for what the two
// documents are; the spec (Documentation/Specs/scene-format-reference.md) for why.
module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"

module editor.mcp;

import foundation.core;
import foundation.json;
import foundation.scene;
import foundation.scene.resource;
import foundation.xml.serialization;
import foundation.resource;
import foundation.script.resource;
import foundation.mcp;
import pipeline.core;
import engine.composition; // the FULL composition: every manager + settings system
import engine.script;      // the example's script behaviour with one override per kind

using namespace foundation::core;
using foundation::json::JsonValue;
namespace scene = foundation::scene;
namespace script = foundation::script;

namespace
{
    // ---- small text helpers ----

    StringView Utf8(const char* text) noexcept
    {
        return text != nullptr ? StringView(reinterpret_cast<const utf8char*>(text)) : StringView();
    }

    String Hex(u64 value)
    {
        static constexpr char kDigits[] = "0123456789ABCDEF";
        utf8char digits[16];
        usize count = 0;
        do
        {
            digits[count++] = static_cast<utf8char>(kDigits[value & 15u]);
            value >>= 4u;
        } while (value != 0);
        utf8char text[18] = {u8'0', u8'x'};
        for (usize i = 0; i < count; ++i)
        {
            text[2 + i] = digits[count - 1 - i];
        }
        return String(StringView(text, 2 + count));
    }

    String Decimal(u64 value) { return Format(u8"{}", value); }

    bool EqualsIgnoreCase(StringView a, StringView b) noexcept
    {
        return a.Size() == b.Size() && a.ContainsIgnoreCase(b);
    }

    StringView KindName(const SchemaNode& node) noexcept
    {
        switch (node.kind)
        {
        case SchemaNodeKind::Object:
            return u8"object";
        case SchemaNodeKind::Array:
            return u8"array";
        case SchemaNodeKind::Text:
            return u8"string";
        case SchemaNodeKind::Guid:
            return u8"guid";
        case SchemaNodeKind::Blob:
            return u8"blob";
        case SchemaNodeKind::Scalar:
            break;
        }
        switch (node.scalar)
        {
        case ScalarKind::Bool:
            return u8"bool";
        case ScalarKind::Int8:
            return u8"i8";
        case ScalarKind::UInt8:
            return u8"u8";
        case ScalarKind::Int16:
            return u8"i16";
        case ScalarKind::UInt16:
            return u8"u16";
        case ScalarKind::Int32:
            return u8"i32";
        case ScalarKind::UInt32:
            return u8"u32";
        case ScalarKind::Int64:
            return u8"i64";
        case ScalarKind::UInt64:
            return u8"u64";
        case ScalarKind::Float32:
            return u8"f32";
        case ScalarKind::Float64:
            return u8"f64";
        }
        return u8"scalar";
    }

    bool IsIntegerScalar(const SchemaNode& node) noexcept
    {
        return node.kind == SchemaNodeKind::Scalar && node.scalar != ScalarKind::Bool &&
               node.scalar != ScalarKind::Float32 && node.scalar != ScalarKind::Float64;
    }

    JsonValue TypeJson(const TypeInfo& type)
    {
        JsonValue out = JsonValue::MakeObject();
        out.Set(u8"name", JsonValue::MakeString(String(Utf8(type.name))));
        out.Set(u8"namespace", JsonValue::MakeString(String(Utf8(type.namespaceName))));
        out.Set(u8"id", JsonValue::MakeString(Decimal(type.id)));
        out.Set(u8"idHex", JsonValue::MakeString(Hex(type.id)));
        return out;
    }

    // A type reflection named (REFLECT_VALUE / REFLECT_ENUM / RTTI_OBJECT), as opposed to the
    // placeholder TypeOf<T>() makes for a value nobody registered.
    bool HasAuthoredName(const TypeInfo& type) noexcept
    {
        return type.namespaceName != nullptr && type.namespaceName[0] != '\0';
    }

    // Whether anything is known about `type`'s fields: a type with no reflected property anywhere
    // in its chain says nothing about its children, so they are not "unreflected", just unknown.
    bool HasReflectedProperties(const TypeInfo* type) noexcept
    {
        for (const TypeInfo* t = type; t != nullptr; t = t->base)
        {
            if (t->propertyCount > 0)
            {
                return true;
            }
        }
        return false;
    }

    // ---- the reference join (D4) ----

    struct Joiner
    {
        const pipeline::BuilderRegistry* builders = nullptr;

        // `ref` for a reference-shaped value type: "entity" for an EntityRef, {resource, asset}
        // for a Ref<T> - the runtime type T, the factory DESCRIPTION the composition carries for
        // it (its cooked form; engine-composition.md D1), the builder that produces that cooked
        // form, its asset type. A factory that reads several cooked forms (a texture's, a render
        // texture's) gives several asset types: `asset` is the first, and `assets` lists them all
        // when there is more than one. Nothing is constructed: the join reads declarations, so
        // every host answers the same. `asset` is omitted (and `resolved` false) only when no
        // builder in this composition produces a cooked form; null for everything else.
        [[nodiscard]] JsonValue RefJson(const TypeInfo& type, bool& resolved) const
        {
            resolved = true;
            if (type.id == TypeOf<scene::EntityRef>().id)
            {
                return JsonValue::MakeString(u8"entity");
            }
            if (type.reference == nullptr || type.reference->Target == nullptr)
            {
                return JsonValue::MakeNull();
            }
            const TypeInfo* target = type.reference->Target();
            if (target == nullptr)
            {
                return JsonValue::MakeNull();
            }
            JsonValue out = JsonValue::MakeObject();
            out.Set(u8"resource", JsonValue::MakeString(String(Utf8(target->name))));
            const Array<const TypeInfo*> assets =
                builders != nullptr ? editor::mcp::SourceAssetTypesFor(*builders, *target) : Array<const TypeInfo*>{};
            if (assets.IsEmpty())
            {
                resolved = false;
                return out;
            }
            out.Set(u8"asset", JsonValue::MakeString(String(Utf8(assets[0]->name))));
            if (assets.Size() > 1)
            {
                JsonValue list = JsonValue::MakeArray();
                for (const TypeInfo* asset : assets)
                {
                    list.Add(JsonValue::MakeString(String(Utf8(asset->name))));
                }
                out.Set(u8"assets", Move(list));
            }
            return out;
        }
    };

    // ---- recorded fields -> JSON, annotated by the reflected twin (D3) ----

    constexpr StringView kReservedFieldKeys[] = {u8"key",     u8"kind", u8"default", u8"fields",
                                                 u8"element", u8"count", u8"blobSize",
                                                 u8"enum",    u8"ref",  u8"elementType"};

    bool IsReservedFieldKey(StringView key) noexcept
    {
        for (StringView reserved : kReservedFieldKeys)
        {
            if (reserved == key)
            {
                return true;
            }
        }
        return false;
    }

    // An f32 as the JSON number an author wrote: its own shortest text read back as f64, so a
    // range step of 0.1f reads 0.1, not its widening 0.10000000149011612.
    JsonValue F32Json(f32 value)
    {
        const String text = Format(u8"{}", value);
        const Optional<f64> parsed = ParseFloat(text.AsView());
        return JsonValue::MakeNumber(parsed.HasValue() ? parsed.Value() : static_cast<f64>(value));
    }

    JsonValue AttributeJson(StringView key, const Variant& value)
    {
        if (const String* text = value.TryGet<String>())
        {
            return JsonValue::MakeString(*text);
        }
        if (const Float4* v = value.TryGet<Float4>())
        {
            if (key == u8"range") // {min, max, step}: the inspector's slider bounds
            {
                JsonValue range = JsonValue::MakeObject();
                range.Set(u8"min", F32Json(v->x));
                range.Set(u8"max", F32Json(v->y));
                range.Set(u8"step", F32Json(v->z));
                return range;
            }
            JsonValue list = JsonValue::MakeArray();
            list.Add(F32Json(v->x));
            list.Add(F32Json(v->y));
            list.Add(F32Json(v->z));
            list.Add(F32Json(v->w));
            return list;
        }
        if (const bool* b = value.TryGet<bool>())
        {
            return JsonValue::MakeBool(*b);
        }
        if (const f32* f = value.TryGet<f32>())
        {
            return F32Json(*f);
        }
        if (const f64* d = value.TryGet<f64>())
        {
            return JsonValue::MakeNumber(*d);
        }
        if (const i32* i = value.TryGet<i32>())
        {
            return JsonValue::MakeNumber(static_cast<f64>(*i));
        }
        if (const u32* u = value.TryGet<u32>())
        {
            return JsonValue::MakeNumber(static_cast<f64>(*u));
        }
        return JsonValue::MakeNull(); // a type the schema has no text for: skipped
    }

    struct FieldContext
    {
        const Joiner* joiner;
        Array<String>* unreflected; // keys with no reflected twin, and refs no factory resolves
    };

    JsonValue FieldsJson(const SchemaNode& parent, const TypeInfo* owner, const FieldContext& ctx);

    // One recorded node as a schema field. `prop` is its reflected twin (null = none);
    // `valueType` the reflected type of the value (the property's type, or a container's
    // element type for an array element).
    JsonValue FieldJson(const SchemaNode& node, const PropertyInfo* prop, const TypeInfo* valueType,
                        const FieldContext& ctx)
    {
        JsonValue field = JsonValue::MakeObject();
        if (!node.key.IsEmpty())
        {
            field.Set(u8"key", JsonValue::MakeString(node.key));
        }
        field.Set(u8"kind", JsonValue::MakeString(String(KindName(node))));
        switch (node.kind)
        {
        case SchemaNodeKind::Scalar:
        case SchemaNodeKind::Text:
        case SchemaNodeKind::Guid:
            field.Set(u8"default", JsonValue::MakeString(node.value));
            break;
        case SchemaNodeKind::Blob:
            field.Set(u8"blobSize", JsonValue::MakeNumber(static_cast<f64>(node.blobSize)));
            break;
        case SchemaNodeKind::Object:
            field.Set(u8"fields", FieldsJson(node, valueType, ctx));
            break;
        case SchemaNodeKind::Array:
        {
            field.Set(u8"count", JsonValue::MakeNumber(static_cast<f64>(node.count)));
            const TypeInfo* elementType =
                (valueType != nullptr && valueType->container != nullptr)
                    ? valueType->container->elementType
                    : nullptr;
            if (elementType != nullptr)
            {
                if (HasAuthoredName(*elementType))
                {
                    field.Set(u8"elementType",
                              JsonValue::MakeString(String(Utf8(elementType->name))));
                }
                bool resolved = true;
                JsonValue ref = ctx.joiner->RefJson(*elementType, resolved);
                if (!ref.IsNull())
                {
                    field.Set(u8"ref", Move(ref));
                }
            }
            // Elements are written inline, no scope of their own: a keyed run of fields per
            // struct element (the run repeats from its first key), one unkeyed node per scalar,
            // string or guid element. The first element stands for all of them.
            if (const SchemaNode* first = node.At(0))
            {
                if (first->key.IsEmpty())
                {
                    field.Set(u8"element", FieldJson(*first, nullptr, elementType, ctx));
                }
                else
                {
                    JsonValue run = JsonValue::MakeArray();
                    for (usize i = 0; i < node.children.Size(); ++i)
                    {
                        const SchemaNode& child = *node.children[i];
                        if (i > 0 && child.key == first->key)
                        {
                            break;
                        }
                        const PropertyInfo* elementProp =
                            elementType != nullptr
                                ? FindProperty(*elementType,
                                               reinterpret_cast<const char*>(child.key.CStr()))
                                : nullptr;
                        if (elementProp == nullptr && HasReflectedProperties(elementType))
                        {
                            ctx.unreflected->PushBack(Format(u8"{}[].{}", node.key.AsView(), child.key.AsView()));
                        }
                        run.Add(FieldJson(child, elementProp,
                                          elementProp != nullptr ? elementProp->type : nullptr, ctx));
                    }
                    JsonValue element = JsonValue::MakeObject();
                    element.Set(u8"fields", Move(run));
                    field.Set(u8"element", Move(element));
                }
            }
            break;
        }
        }

        if (prop != nullptr)
        {
            const TypeInfo& type = *prop->type;
            if (type.enumeratorCount > 0 && IsIntegerScalar(node))
            {
                JsonValue names = JsonValue::MakeArray();
                for (u32 i = 0; i < type.enumeratorCount; ++i)
                {
                    JsonValue entry = JsonValue::MakeObject();
                    entry.Set(u8"name", JsonValue::MakeString(String(Utf8(type.enumerators[i].name))));
                    entry.Set(u8"value",
                              JsonValue::MakeNumber(static_cast<f64>(type.enumerators[i].value)));
                    names.Add(Move(entry));
                }
                field.Set(u8"enum", Move(names));
            }
            for (const Attribute& attribute : Attributes(*prop))
            {
                const StringView key = Utf8(attribute.key);
                if (IsReservedFieldKey(key))
                {
                    continue;
                }
                JsonValue value = AttributeJson(key, attribute.value);
                if (!value.IsNull())
                {
                    field.Set(String(key), Move(value));
                }
            }
            bool resolved = true;
            JsonValue ref = ctx.joiner->RefJson(type, resolved);
            if (!ref.IsNull())
            {
                field.Set(u8"ref", Move(ref));
            }
            if (!resolved)
            {
                ctx.unreflected->PushBack(node.key);
            }
        }
        return field;
    }

    // `parent`'s children as the schema's `fields` array, each joined to `owner`'s reflected
    // property of the same name (owner null = nothing known about this level).
    JsonValue FieldsJson(const SchemaNode& parent, const TypeInfo* owner, const FieldContext& ctx)
    {
        JsonValue fields = JsonValue::MakeArray();
        for (const UniquePtr<SchemaNode>& child : parent.children)
        {
            const PropertyInfo* prop =
                owner != nullptr ? FindProperty(*owner, reinterpret_cast<const char*>(child->key.CStr()))
                                 : nullptr;
            if (prop == nullptr && HasReflectedProperties(owner))
            {
                ctx.unreflected->PushBack(child->key);
            }
            fields.Add(FieldJson(*child, prop, prop != nullptr ? prop->type : nullptr, ctx));
        }
        return fields;
    }

    JsonValue StringsJson(const Array<String>& items)
    {
        JsonValue out = JsonValue::MakeArray();
        for (const String& item : items)
        {
            out.Add(JsonValue::MakeString(item));
        }
        return out;
    }

    // The chain BeginVersionedPayload wrote: the concrete type first, then every versioned base.
    JsonValue DataVersionsJson(Span<const SerializedDataVersion> chain, const TypeInfo& concrete)
    {
        JsonValue out = JsonValue::MakeArray();
        for (const SerializedDataVersion& entry : chain)
        {
            const TypeInfo* named = nullptr;
            for (const TypeInfo* t = &concrete; t != nullptr && named == nullptr; t = t->base)
            {
                if (t->id == entry.typeId)
                {
                    named = t;
                }
            }
            if (named == nullptr)
            {
                named = GlobalTypeRegistry().FindById(entry.typeId);
            }
            JsonValue row = JsonValue::MakeObject();
            row.Set(u8"type", JsonValue::MakeString(Decimal(entry.typeId)));
            row.Set(u8"typeHex", JsonValue::MakeString(Hex(entry.typeId)));
            if (named != nullptr)
            {
                row.Set(u8"typeName",
                        JsonValue::MakeString(Format(u8"{}::{}", Utf8(named->namespaceName),
                                                     Utf8(named->name))));
            }
            row.Set(u8"version", JsonValue::MakeNumber(static_cast<f64>(entry.version)));
            out.Add(Move(row));
        }
        return out;
    }

    // The recorded body as fields, minus the `dataVersions` array a versioned payload puts first.
    JsonValue BodyFieldsJson(const SchemaNode& root, const TypeInfo* owner, const FieldContext& ctx)
    {
        usize firstField = 0;
        if (const SchemaNode* first = root.At(0);
            first != nullptr && first->key.AsView() == u8"dataVersions")
        {
            firstField = 1;
        }
        JsonValue fields = JsonValue::MakeArray();
        for (usize i = firstField; i < root.children.Size(); ++i)
        {
            const SchemaNode& child = *root.children[i];
            const PropertyInfo* prop =
                owner != nullptr ? FindProperty(*owner, reinterpret_cast<const char*>(child.key.CStr()))
                                 : nullptr;
            if (prop == nullptr && HasReflectedProperties(owner))
            {
                ctx.unreflected->PushBack(child.key);
            }
            fields.Add(FieldJson(child, prop, prop != nullptr ? prop->type : nullptr, ctx));
        }
        return fields;
    }

    // ---- the example scene (D2, D5) ----

    struct Example
    {
        scene::EntityHandle root;
        scene::EntityHandle child;
        HashMap<u64, scene::EntityHandle> entityByManager; // the entity carrying each manager's component
    };

    script::ScriptPropertyValue SampleValue(script::ScriptPropertyType kind, const Guid& child,
                                            Random& rng)
    {
        script::ScriptPropertyValue value;
        value.kind = kind;
        switch (kind)
        {
        case script::ScriptPropertyType::Float:
            value.number = 1.5;
            break;
        case script::ScriptPropertyType::Int:
            value.number = 3.0;
            break;
        case script::ScriptPropertyType::Bool:
            value.boolean = true;
            break;
        case script::ScriptPropertyType::String:
            value.text = String(u8"hello");
            break;
        case script::ScriptPropertyType::Color:
            value.color = Color{1.0f, 0.5f, 0.25f, 1.0f};
            break;
        case script::ScriptPropertyType::Vec3:
            value.vector = Float3{1.0f, 2.0f, 3.0f};
            break;
        case script::ScriptPropertyType::Entity:
            value.guid = child;
            break;
        case script::ScriptPropertyType::Asset:
            value.guid = Guid::Generate(rng);
            break;
        case script::ScriptPropertyType::None:
            break;
        }
        return value;
    }

    // Root and Child (the hierarchy pair), then one entity per serializable manager carrying that
    // component at its defaults. Guids come from a seeded generator, in creation order.
    Example ComposeExample(scene::Scene& world, Random& rng)
    {
        Example example;
        example.root = world.CreateEntity(Guid::Generate(rng), u8"Root");
        example.child = world.CreateEntity(Guid::Generate(rng), u8"Child");
        world.SetParent(example.child, example.root);
        world.ForEachManager(
            [&](scene::ComponentManagerBase& manager)
            {
                if (!manager.IsSerializable())
                {
                    return;
                }
                const scene::EntityHandle entity =
                    world.CreateEntity(Guid::Generate(rng), manager.SerializationTypeId());
                world.SetParent(entity, example.root);
                if (manager.AddDefaultComponent(entity))
                {
                    example.entityByManager.InsertOrAssign(manager.ComponentType()->id, entity);
                }
            });
        return example;
    }

    // After the schema recorded the defaults: the script component's entity gets one behaviour
    // with an override of every property kind (named "<kind>Value"), so the example shows the
    // override encoding the scriptOverrides section describes.
    void FillScriptBehaviour(scene::Scene& world, const Example& example, Random& rng)
    {
        auto* scripts = world.GetSystem<engine::script::ScriptComponentManager>();
        if (scripts == nullptr)
        {
            return;
        }
        const scene::EntityHandle* entity =
            example.entityByManager.Find(scripts->ComponentType()->id);
        engine::script::ScriptComponent* component =
            entity != nullptr ? scripts->Get(*entity) : nullptr;
        if (component == nullptr)
        {
            return;
        }
        const Guid childId = world.GetEntityId(example.child);
        engine::script::ScriptBehavior behaviour;
        behaviour.script.SetId(Guid::Generate(rng));
        for (const script::ScriptPropertyTypeEntry& entry : script::ScriptPropertyTypeNames())
        {
            const String name = Format(u8"{}Value", entry.name);
            behaviour.SetOverride(script::ScriptPropertyNameHash(name.AsView()),
                                  SampleValue(entry.kind, childId, rng));
        }
        component->behaviors.PushBack(Move(behaviour));
    }

    // ---- the schema's sections ----

    JsonValue FormatSection(const SchemaNode& stream, const SchemaNode& prefabRecord)
    {
        JsonValue out = JsonValue::MakeObject();
        out.Set(u8"magic", JsonValue::MakeString(Decimal(scene::detail::kSceneStreamMagic)));
        out.Set(u8"magicHex", JsonValue::MakeString(Hex(scene::detail::kSceneStreamMagic)));
        out.Set(u8"version", JsonValue::MakeNumber(static_cast<f64>(scene::detail::kSceneStreamVersion)));
        out.Set(u8"sourceEncoding", JsonValue::MakeString(u8"xml"));
        JsonValue sections = JsonValue::MakeArray();
        for (const UniquePtr<SchemaNode>& child : stream.children)
        {
            JsonValue section = JsonValue::MakeObject();
            section.Set(u8"key", JsonValue::MakeString(child->key));
            section.Set(u8"kind", JsonValue::MakeString(String(KindName(*child))));
            sections.Add(Move(section));
        }
        out.Set(u8"sections", Move(sections));
        JsonValue prefabMode = JsonValue::MakeObject();
        prefabMode.Set(u8"referenced",
                       JsonValue::MakeNumber(static_cast<f64>(scene::detail::kPrefabWireReferenced3)));
        prefabMode.Set(u8"expanded",
                       JsonValue::MakeNumber(static_cast<f64>(scene::detail::kPrefabWireExpanded2)));
        out.Set(u8"prefabModeValues", Move(prefabMode));
        out.Set(u8"arrays",
                JsonValue::MakeString(
                    u8"an array carries its element count and writes the elements inline with no "
                    u8"scope of their own: a struct element is the run of its keyed fields (the run "
                    u8"repeats from its first key), a scalar, string or guid element one unkeyed "
                    u8"value. Component and settings records are the exception: one object per "
                    u8"record (componentRecord, settingsRecord)"));
        out.Set(u8"structs",
                JsonValue::MakeString(
                    u8"a math value (Float2/3/4, Quaternion, Color) is an object of its named "
                    u8"components; any other struct field writes its fields inline under the "
                    u8"parent, without a key of its own"));

        Joiner none;
        Array<String> ignored;
        FieldContext ctx{&none, &ignored};
        // The entity record: the first entity's run of fields (the example's Root).
        if (const SchemaNode* entities = stream.Find(u8"entities"))
        {
            if (const SchemaNode* first = entities->At(0))
            {
                JsonValue record = JsonValue::MakeArray();
                for (usize i = 0; i < entities->children.Size(); ++i)
                {
                    const SchemaNode& child = *entities->children[i];
                    if (i > 0 && child.key == first->key)
                    {
                        break;
                    }
                    record.Add(FieldJson(child, nullptr, nullptr, ctx));
                }
                out.Set(u8"entityRecord", Move(record));
            }
        }
        // The component record framing, its `data` standing for the component's own fields.
        if (const SchemaNode* components = stream.Find(u8"components"))
        {
            if (const SchemaNode* first = components->At(0))
            {
                JsonValue record = JsonValue::MakeArray();
                for (const UniquePtr<SchemaNode>& child : first->children)
                {
                    JsonValue field = JsonValue::MakeObject();
                    field.Set(u8"key", JsonValue::MakeString(child->key));
                    field.Set(u8"kind", JsonValue::MakeString(String(KindName(*child))));
                    if (child->key.AsView() == u8"data")
                    {
                        field.Set(u8"fields",
                                  JsonValue::MakeString(
                                      u8"the component's dataVersions then its fields: components[].fields"));
                    }
                    else if (child->key.AsView() == u8"type")
                    {
                        field.Set(u8"value", JsonValue::MakeString(u8"the component's wireName"));
                    }
                    else if (child->key.AsView() == u8"owner")
                    {
                        field.Set(u8"value", JsonValue::MakeString(u8"the owning entity's id"));
                    }
                    record.Add(Move(field));
                }
                out.Set(u8"componentRecord", Move(record));
            }
        }
        if (const SchemaNode* settings = stream.Find(u8"systemSettings"))
        {
            if (const SchemaNode* first = settings->At(0))
            {
                JsonValue record = JsonValue::MakeArray();
                for (const UniquePtr<SchemaNode>& child : first->children)
                {
                    JsonValue field = JsonValue::MakeObject();
                    field.Set(u8"key", JsonValue::MakeString(child->key));
                    field.Set(u8"kind", JsonValue::MakeString(String(KindName(*child))));
                    if (child->key.AsView() == u8"settings")
                    {
                        field.Set(u8"fields", JsonValue::MakeString(u8"the block's fields: settings[].fields"));
                    }
                    else if (child->key.AsView() == u8"system")
                    {
                        field.Set(u8"value", JsonValue::MakeString(u8"the block's system id"));
                    }
                    else if (child->key.AsView() == u8"dataVersions")
                    {
                        field.Set(u8"value", JsonValue::MakeString(u8"the block's dataVersions"));
                    }
                    record.Add(Move(field));
                }
                out.Set(u8"settingsRecord", Move(record));
            }
        }
        // The prefab instance record: the writer's run of keys for one prefabInstances element
        // (recorded from a parked instance, since the example has no prefab to spawn), with what
        // an author puts in the keys a placement needs.
        {
            JsonValue fields = FieldsJson(prefabRecord, nullptr, ctx);
            JsonValue described = JsonValue::MakeArray();
            for (i64 i = 0; i < fields.Count(); ++i)
            {
                JsonValue field = fields.At(i);
                const String key = field.Get(u8"key").AsString();
                const StringView k = key.AsView();
                if (k == u8"prefab")
                {
                    field.Set(u8"value", JsonValue::MakeString(
                                             u8"the prefab asset's guid (asset_list: type "
                                             u8"PrefabDocument; an imported model's is the `Prefab` "
                                             u8"beside its manifest)"));
                }
                else if (k == u8"parent")
                {
                    field.Set(u8"value", JsonValue::MakeString(u8"the entity the instance root "
                                                               u8"sits under; nil for a scene "
                                                               u8"root"));
                }
                else if (k == u8"rootLive")
                {
                    field.Set(u8"value", JsonValue::MakeString(u8"the instance root's entity id "
                                                               u8"as last saved; any fresh guid "
                                                               u8"when authoring"));
                }
                else if (k == u8"placement")
                {
                    field.Set(u8"value", JsonValue::MakeString(u8"1: the transform places the "
                                                               u8"root"));
                }
                else if (k == u8"members")
                {
                    field.Set(u8"value", JsonValue::MakeString(
                                             u8"the prefab's entity ids paired with the "
                                             u8"instance's; EMPTY when authoring: the load mints "
                                             u8"fresh ids, and a save records them"));
                }
                described.Add(Move(field));
            }
            JsonValue record = JsonValue::MakeObject();
            record.Set(u8"mode",
                       JsonValue::MakeString(Format(
                           u8"prefabMode must be {} (referenced) for prefabInstances to be read; "
                           u8"each element is a run of these keys, in this order, laid inline in "
                           u8"the array. Authoring a placement: the prefab, the root's parent and "
                           u8"transform, a fresh rootLive, nil owner, nestedSrcRoot and "
                           u8"nextSibling, and every array empty; the load spawns it and a save "
                           u8"fills the arrays in",
                           static_cast<u32>(scene::detail::kPrefabWireReferenced3))));
            record.Set(u8"fields", Move(described));
            out.Set(u8"prefabInstanceRecord", Move(record));
        }
        return out;
    }

    JsonValue ComponentsSection(IAllocator& allocator, scene::Scene& world, const Example& example,
                                const Joiner& joiner)
    {
        JsonValue out = JsonValue::MakeArray();
        world.ForEachManager(
            [&](scene::ComponentManagerBase& manager)
            {
                if (!manager.IsSerializable())
                {
                    return;
                }
                const TypeInfo& type = *manager.ComponentType();
                JsonValue entry = JsonValue::MakeObject();
                entry.Set(u8"wireName", JsonValue::MakeString(String(manager.SerializationTypeId())));
                entry.Set(u8"type", TypeJson(type));
                const scene::EntityHandle* entity = example.entityByManager.Find(type.id);
                if (entity == nullptr)
                {
                    entry.Set(u8"recorded", JsonValue::MakeBool(false));
                    out.Add(Move(entry));
                    return;
                }
                SchemaRecorder recorder(allocator);
                manager.WriteComponent(recorder, *entity);
                entry.Set(u8"dataVersions", DataVersionsJson(recorder.VersionChain(), type));
                Array<String> unreflected;
                FieldContext ctx{&joiner, &unreflected};
                entry.Set(u8"fields", BodyFieldsJson(recorder.Root(), &type, ctx));
                entry.Set(u8"unreflected", StringsJson(unreflected));
                out.Add(Move(entry));
            });
        return out;
    }

    JsonValue SettingsSection(IAllocator& allocator, scene::Scene& world, const Joiner& joiner)
    {
        JsonValue out = JsonValue::MakeArray();
        world.ForEachSystem(
            [&](scene::SceneSystem& system)
            {
                const TypeInfo* type = system.SettingsType();
                if (type == nullptr)
                {
                    return;
                }
                // Exactly the framing SerializeScene writes for a text settings record.
                SchemaRecorder recorder(allocator);
                foundation::core::BeginVersionedPayload(recorder, *type);
                recorder.Key("settings");
                recorder.BeginObject();
                system.SerializeSettings(recorder);
                recorder.EndObject();
                foundation::core::EndVersionedPayload(recorder);

                JsonValue entry = JsonValue::MakeObject();
                entry.Set(u8"system", JsonValue::MakeString(String(system.SettingsId())));
                entry.Set(u8"type", TypeJson(*type));
                entry.Set(u8"dataVersions", DataVersionsJson(recorder.VersionChain(), *type));
                Array<String> unreflected;
                FieldContext ctx{&joiner, &unreflected};
                const SchemaNode* settings = recorder.Root().Find(u8"settings");
                entry.Set(u8"fields", settings != nullptr ? FieldsJson(*settings, type, ctx)
                                                          : JsonValue::MakeArray());
                entry.Set(u8"unreflected", StringsJson(unreflected));
                out.Add(Move(entry));
            });
        return out;
    }

    JsonValue ScriptOverridesSection(IAllocator& allocator)
    {
        JsonValue out = JsonValue::MakeObject();
        Joiner none;
        Array<String> ignored;
        FieldContext ctx{&none, &ignored};

        // The override record as the ScriptComponent writes it.
        {
            SchemaRecorder recorder(allocator);
            engine::script::ScriptPropertyOverride record;
            Serialize(recorder, record);
            out.Set(u8"record", FieldsJson(recorder.Root(), nullptr, ctx));
        }
        // The hash, with a worked example a reader can check.
        {
            JsonValue hash = JsonValue::MakeObject();
            // The numbers are the ones the hash uses, interpolated, never retyped: a retyped
            // basis once documented the standard value while the code used another.
            hash.Set(u8"algorithm",
                     JsonValue::MakeString(Format(u8"FNV-1a, 64-bit, over the UTF-8 bytes of the "
                                                  u8"property name: offset basis {}, prime {}, no "
                                                  u8"terminator",
                                                  kFnv1a64OffsetBasis, kFnv1a64Prime)));
            hash.Set(u8"offsetBasis", JsonValue::MakeString(Decimal(kFnv1a64OffsetBasis)));
            hash.Set(u8"prime", JsonValue::MakeString(Decimal(kFnv1a64Prime)));
            const StringView exampleName = u8"speed";
            JsonValue example = JsonValue::MakeObject();
            example.Set(u8"name", JsonValue::MakeString(String(exampleName)));
            example.Set(u8"nameHash",
                        JsonValue::MakeString(Decimal(script::ScriptPropertyNameHash(exampleName))));
            hash.Set(u8"example", Move(example));
            out.Set(u8"hash", Move(hash));
        }
        // Every kind: its number on the wire, its authored spelling, and the payload key the
        // value uses - taken from the value's own Serialize, one kind at a time.
        {
            JsonValue kinds = JsonValue::MakeArray();
            for (const script::ScriptPropertyTypeEntry& entry : script::ScriptPropertyTypeNames())
            {
                script::ScriptPropertyValue value;
                value.kind = entry.kind;
                SchemaRecorder recorder(allocator);
                Serialize(recorder, value);
                JsonValue kind = JsonValue::MakeObject();
                kind.Set(u8"kind", JsonValue::MakeNumber(static_cast<f64>(static_cast<u8>(entry.kind))));
                kind.Set(u8"name", JsonValue::MakeString(String(entry.name)));
                if (entry.kind == script::ScriptPropertyType::Asset)
                {
                    kind.Set(u8"authoredAs",
                             JsonValue::MakeString(Format(u8"{}<TypeName>", script::kScriptAssetTypePrefix)));
                }
                JsonValue payload = JsonValue::MakeArray();
                for (const UniquePtr<SchemaNode>& field : recorder.Root().children)
                {
                    if (field->key.AsView() != u8"kind")
                    {
                        JsonValue key = JsonValue::MakeObject();
                        key.Set(u8"key", JsonValue::MakeString(field->key));
                        key.Set(u8"kind", JsonValue::MakeString(String(KindName(*field))));
                        if (field->kind == SchemaNodeKind::Object)
                        {
                            key.Set(u8"fields", FieldsJson(*field, nullptr, ctx));
                        }
                        payload.Add(Move(key));
                    }
                }
                kind.Set(u8"payload", Move(payload));
                kinds.Add(Move(kind));
            }
            out.Set(u8"kinds", Move(kinds));
        }
        return out;
    }

    JsonValue EntryByName(const JsonValue& entries, StringView name, StringView wireKey)
    {
        for (const JsonValue& entry : entries.Items())
        {
            if (EqualsIgnoreCase(entry.Get(String(wireKey)).AsString().AsView(), name) ||
                EqualsIgnoreCase(entry.Get(u8"type").Get(u8"name").AsString().AsView(), name))
            {
                return entry;
            }
        }
        return JsonValue::MakeNull();
    }

    String EntryNames(const JsonValue& entries, StringView wireKey)
    {
        String names;
        for (const JsonValue& entry : entries.Items())
        {
            if (!names.IsEmpty())
            {
                names += u8", ";
            }
            names += entry.Get(String(wireKey)).AsString().AsView();
        }
        return names;
    }
}

namespace editor::mcp
{
    Array<const TypeInfo*> SourceAssetTypesFor(const pipeline::BuilderRegistry& builders,
                                               const TypeInfo& product)
    {
        Array<const TypeInfo*> cookedForms;
        engine::FullComposition().ForEachFactoryDescription(
            [&](const foundation::resource::ResourceModule&,
                const foundation::resource::ResourceFactoryDesc& desc)
            {
                const TypeInfo* made = desc.product();
                if (cookedForms.IsEmpty() && made != nullptr && made->id == product.id)
                {
                    desc.ForEachCooked([&](const TypeInfo* cooked) { cookedForms.PushBack(cooked); });
                }
            });
        Array<const TypeInfo*> assets;
        if (cookedForms.IsEmpty())
        {
            // Nothing makes it at run time: it may be a cooked form itself (a collision shape's
            // mesh names what a mesh cooks to). Every asset whose product is one, or derives from
            // one (a skinned mesh's source is a static mesh's), makes it.
            builders.ForEach(
                [&](const pipeline::IAssetBuilder& builder)
                {
                    const TypeInfo* output = builder.ProductType();
                    const TypeInfo* asset = builder.AssetType();
                    if (output != nullptr && asset != nullptr && (output->id == product.id || IsDerivedFrom(output, &product)))
                        assets.PushBack(asset); // each builder its own asset type
                });
            return assets;
        }
        for (const TypeInfo* cooked : cookedForms)
        {
            const TypeInfo* asset = nullptr;
            builders.ForEach(
                [&](const pipeline::IAssetBuilder& builder)
                {
                    const TypeInfo* output = builder.ProductType();
                    if (asset == nullptr && cooked != nullptr && output != nullptr &&
                        output->id == cooked->id)
                    {
                        asset = builder.AssetType();
                    }
                });
            if (asset != nullptr)
            {
                assets.PushBack(asset);
            }
        }
        return assets;
    }

    SceneReference GenerateSceneReference(IAllocator& allocator,
                                          const pipeline::BuilderRegistry& builders)
    {
        engine::RegisterAllSceneComponentReflection(); // idempotent; the joins need the metadata
        SceneReference reference;

        scene::Scene world(allocator, u8"SceneReference");
        engine::AddAllSceneManagers(world);
        Random rng(0x5CE7EF0247A7ull); // fixed: the example's guids reproduce run after run
        const Example example = ComposeExample(world, rng);

        // The schema's bodies first, over the DEFAULTS (D1), before the example gets its content.
        Joiner joiner{&builders};
        JsonValue schema = JsonValue::MakeObject();
        JsonValue components = ComponentsSection(allocator, world, example, joiner);
        JsonValue settings = SettingsSection(allocator, world, joiner);
        FillScriptBehaviour(world, example, rng);

        // The example: the very text SaveScene stores.
        {
            foundation::xml::XmlSerializer xml(allocator);
            scene::SerializeScene(xml, world, scene::ScenePrefabMode::Referenced, true,
                                  scene::detail::SceneStreamEncoding::Text);
            if (xml.IsOk())
            {
                xml.GetOutput(reference.exampleXml);
            }
            else
            {
                LOG_ERROR(u8"Editor.Mcp", u8"scene reference: the example scene did not serialize");
            }
        }

        // The framing: the whole example stream recorded once, and one prefab instance record
        // through the same writer, every array but the component ops holding an element so
        // their shapes show.
        {
            SchemaRecorder stream(allocator);
            scene::SerializeScene(stream, world, scene::ScenePrefabMode::Referenced, true,
                                  scene::detail::SceneStreamEncoding::Text);
            scene::Scene::PendingPrefabInstance probe;
            probe.sourceIds.PushBack(Guid{});
            probe.liveIds.PushBack(Guid{});
            probe.destroyedMembers.PushBack(Guid{});
            probe.overrideTransformIds.PushBack(Guid{});
            probe.overrideTransforms.PushBack(Transform{});
            SchemaRecorder prefabRecord(allocator);
            scene::detail::WritePrefabRecord(prefabRecord, world, probe, /*text=*/true);
            schema.Set(u8"format", FormatSection(stream.Root(), prefabRecord.Root()));
        }
        schema.Set(u8"components", Move(components));
        schema.Set(u8"settings", Move(settings));
        schema.Set(u8"scriptOverrides", ScriptOverridesSection(allocator));
        reference.schemaJson = schema.ToString(/*pretty=*/true);
        reference.schema = Move(schema);
        return reference;
    }

    JsonValue FindSchemaEntry(const JsonValue& schema, StringView name)
    {
        JsonValue found = EntryByName(schema.Get(u8"components"), name, u8"wireName");
        if (found.IsNull())
        {
            found = EntryByName(schema.Get(u8"settings"), name, u8"system");
        }
        return found;
    }

    void RegisterSceneReferenceResources(foundation::mcp::McpServer& server,
                                         const SceneReference& reference)
    {
        server.RegisterResource(
            String(kSceneExampleUri), String(u8"SceneExample.scene.xml"), String(u8"application/xml"),
            String(u8"generated by this host: a complete example scene - every component at its "
                   u8"defaults, every settings block, a parent and child, a script behaviour with "
                   u8"one override per property kind - exactly what scene_write accepts"),
            [xml = reference.exampleXml]() -> Result<String, String> { return xml; });
        server.RegisterResource(
            String(kSceneSchemaUri), String(u8"SceneSchema.json"), String(u8"application/json"),
            String(u8"generated by this host: the scene format schema - stream framing, every "
                   u8"component's and settings block's fields in wire order with kinds, defaults, "
                   u8"enum names, ranges and reference targets, and the script override encoding"),
            [json = reference.schemaJson]() -> Result<String, String> { return json; });
    }

    void RegisterComponentSchemaTool(foundation::mcp::McpServer& server,
                                     const SceneReference& reference)
    {
        using foundation::mcp::SchemaBuilder;
        using foundation::mcp::ToolResult;
        server.RegisterTool(
            u8"component_schema",
            u8"One component's or settings block's schema entry from the generated scene format "
            u8"reference (docs://generated/SceneSchema.json): its fields in wire order with kinds, "
            u8"defaults, enum names, ranges and reference targets, plus its dataVersions. Pass "
            u8"`type`: a component's wire name (a record's `type`, e.g. \"light\") or reflected "
            u8"type name, or a settings block's system id.",
            SchemaBuilder()
                .Str(u8"type", u8"component wire name / type name, or settings system id", true)
                .Build(),
            foundation::mcp::ToolAnnotations::ReadOnly(),
            [schema = reference.schema](const JsonValue& args) -> ToolResult
            {
                const String name = args.Get(u8"type").AsString();
                if (name.IsEmpty())
                {
                    return Err(String(u8"`type` is required"));
                }
                JsonValue found = FindSchemaEntry(schema, name.AsView());
                if (found.IsNull())
                {
                    return Err(Format(u8"no component or settings block named '{}'. Components: {}. "
                                      u8"Settings: {}",
                                      name.AsView(),
                                      EntryNames(schema.Get(u8"components"), u8"wireName").AsView(),
                                      EntryNames(schema.Get(u8"settings"), u8"system").AsView()));
                }
                return found;
            });
    }
}
