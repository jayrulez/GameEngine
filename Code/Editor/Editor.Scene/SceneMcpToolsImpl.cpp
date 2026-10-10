// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell
// Editor::Scene - the scene editor's MCP tools' bodies.
module;
#include "Core/Prelude.h"

module editor.scene;

import foundation.core;
import foundation.json;
import foundation.mcp;
import foundation.scene;
import foundation.scene.resource;
import editor.core;
import editor.camera;
import editor.navigation; // BakeNavigationZone (navigation_bake)
import engine.navigation; // NavMeshZoneComponent

using namespace foundation::core;
using foundation::json::JsonValue;
using foundation::mcp::SchemaBuilder;
using foundation::mcp::ToolAnnotations;
using foundation::mcp::ToolResult;
namespace scene = foundation::scene;

namespace editor
{
    namespace
    {

        /// The page's selection as the agent sees it: the page, the entities in order (the first
        /// is the primary, the gizmo pivot), each with its name.
        JsonValue SelectionJson(const AddressedPage& addressed)
        {
            SceneEditContext& edit = addressed.scene->EditContext();
            JsonValue entities = JsonValue::MakeArray();
            for (const Guid& id : edit.EntitySelection().Items())
            {
                JsonValue entry = JsonValue::MakeObject();
                entry.Set(u8"guid", GuidJson(id));
                const scene::EntityHandle handle = edit.Resolve(id);
                entry.Set(u8"name", JsonValue::MakeString(String(
                                        edit.Scene().IsValid(handle)
                                            ? edit.Scene().GetEntityName(handle)
                                            : StringView())));
                entities.Add(Move(entry));
            }
            JsonValue out = JsonValue::MakeObject();
            out.Set(u8"page", PageJson(*addressed.page));
            const Guid* primary = edit.EntitySelection().Primary();
            out.Set(u8"primary", primary != nullptr ? GuidJson(*primary) : JsonValue::MakeNull());
            out.Set(u8"entities", Move(entities));
            return out;
        }

        JsonValue SimulationJson(const AddressedPage& addressed)
        {
            JsonValue out = JsonValue::MakeObject();
            out.Set(u8"page", PageJson(*addressed.page));
            out.Set(u8"simulating", JsonValue::MakeBool(addressed.scene->IsSimulating()));
            return out;
        }

        /// entity_inspect over a running PIE instance: `entity` by guid, name or path in the
        /// scene that game is in now.
        ToolResult InspectPie(EditorContext& context, const JsonValue& args)
        {
            Result<EditorPage*, String> resolved = ResolvePie(context, args);
            if (!resolved.HasValue())
            {
                return Err(Move(resolved.Error()));
            }
            IPieInstancePage* pie = ServiceOf<IPieInstancePage>(resolved.Value());
            scene::Scene* running = pie->RunningScene();
            if (!pie->IsRunning() || running == nullptr)
            {
                return Err(Format(u8"PIE instance '{}' is not running a scene (pie_state says "
                                  u8"where it is)",
                                  pie->PieId()));
            }
            const JsonValue entityArg = args.Get(u8"entity");
            if (!entityArg.IsString())
            {
                return Err(String(u8"pass `entity` (a guid, a name or a slash path): a running "
                                  u8"game has no selection"));
            }
            const String text = entityArg.AsString();
            const scene::EntityHandle handle = FindEntity(*running, text.AsView());
            if (!running->IsValid(handle))
            {
                return Err(Format(u8"no entity '{}' in PIE instance '{}''s scene '{}'",
                                  text.AsView(), pie->PieId(), running->Name()));
            }
            JsonValue out = JsonValue::MakeObject();
            out.Set(u8"pie", JsonValue::MakeString(String(pie->PieId())));
            out.Set(u8"scene", JsonValue::MakeString(String(running->Name())));
            out.Set(u8"entity", EntityJson(*running, handle, context.Resources()));
            return out;
        }

        // === component_set: one property, one locked undo step, through the edit context ===

        /// A JSON value as the Variant a leaf property of `type` takes; empty when the JSON
        /// has the wrong shape (the refusal names the type).
        Variant LeafVariant(const TypeInfo& type, const JsonValue& value)
        {
            const auto number = [&](auto make) -> Variant
            { return value.IsNumber() ? make(value.AsNumber()) : Variant{}; };
            if (&type == &TypeOf<bool>())
            {
                return value.IsBool() ? Variant::From<bool>(value.AsBool()) : Variant{};
            }
            if (&type == &TypeOf<f32>())
            {
                return number([](f64 n) { return Variant::From<f32>(static_cast<f32>(n)); });
            }
            if (&type == &TypeOf<f64>())
            {
                return number([](f64 n) { return Variant::From<f64>(n); });
            }
            if (&type == &TypeOf<i32>())
            {
                return number([](f64 n) { return Variant::From<i32>(static_cast<i32>(n)); });
            }
            if (&type == &TypeOf<u32>())
            {
                return number([](f64 n) { return Variant::From<u32>(static_cast<u32>(n)); });
            }
            if (&type == &TypeOf<i64>())
            {
                return number([](f64 n) { return Variant::From<i64>(static_cast<i64>(n)); });
            }
            if (&type == &TypeOf<u64>())
            {
                return number([](f64 n) { return Variant::From<u64>(static_cast<u64>(n)); });
            }
            if (&type == &TypeOf<i16>())
            {
                return number([](f64 n) { return Variant::From<i16>(static_cast<i16>(n)); });
            }
            if (&type == &TypeOf<u16>())
            {
                return number([](f64 n) { return Variant::From<u16>(static_cast<u16>(n)); });
            }
            if (&type == &TypeOf<i8>())
            {
                return number([](f64 n) { return Variant::From<i8>(static_cast<i8>(n)); });
            }
            if (&type == &TypeOf<u8>())
            {
                return number([](f64 n) { return Variant::From<u8>(static_cast<u8>(n)); });
            }
            if (&type == &TypeOf<String>())
            {
                return value.IsString() ? Variant::From<String>(value.AsString()) : Variant{};
            }
            if (&type == &TypeOf<Guid>())
            {
                Guid id;
                return (value.IsString() && Guid::TryParse(value.AsString().AsView(), id))
                           ? Variant::From<Guid>(id)
                           : Variant{};
            }
            const auto component = [&](usize i) -> f32
            { return static_cast<f32>(value.At(static_cast<i32>(i)).AsNumber()); };
            const auto numbers = [&](usize count)
            {
                if (!value.IsArray() || static_cast<usize>(value.Count()) != count)
                {
                    return false;
                }
                for (usize i = 0; i < count; ++i)
                {
                    if (!value.At(static_cast<i32>(i)).IsNumber())
                    {
                        return false;
                    }
                }
                return true;
            };
            if (&type == &TypeOf<Float2>())
            {
                return numbers(2) ? Variant::From<Float2>(Float2{component(0), component(1)}) : Variant{};
            }
            if (&type == &TypeOf<Float3>())
            {
                return numbers(3) ? Variant::From<Float3>(Float3{component(0), component(1), component(2)})
                                  : Variant{};
            }
            if (&type == &TypeOf<Float4>())
            {
                return numbers(4) ? Variant::From<Float4>(
                                        Float4{component(0), component(1), component(2), component(3)})
                                  : Variant{};
            }
            if (&type == &TypeOf<Quaternion>())
            {
                return numbers(4) ? Variant::From<Quaternion>(
                                        Quaternion{component(0), component(1), component(2), component(3)})
                                  : Variant{};
            }
            if (&type == &TypeOf<Color>())
            {
                return numbers(4) ? Variant::From<Color>(
                                        Color{component(0), component(1), component(2), component(3)})
                                  : Variant{};
            }
            return Variant{};
        }

        /// The JSON shape a leaf property takes, for a refusal that teaches.
        StringView LeafShape(const TypeInfo& type)
        {
            if (&type == &TypeOf<bool>())
            {
                return u8"a boolean";
            }
            if (&type == &TypeOf<String>())
            {
                return u8"a string";
            }
            if (&type == &TypeOf<Guid>())
            {
                return u8"a guid string";
            }
            if (&type == &TypeOf<Float2>())
            {
                return u8"[x, y]";
            }
            if (&type == &TypeOf<Float3>())
            {
                return u8"[x, y, z]";
            }
            if (&type == &TypeOf<Float4>() || &type == &TypeOf<Quaternion>())
            {
                return u8"[x, y, z, w]";
            }
            if (&type == &TypeOf<Color>())
            {
                return u8"[r, g, b, a]";
            }
            if (&type == &TypeOf<f32>() || &type == &TypeOf<f64>() || &type == &TypeOf<i32>() ||
                &type == &TypeOf<u32>() || &type == &TypeOf<i64>() || &type == &TypeOf<u64>() ||
                &type == &TypeOf<i16>() || &type == &TypeOf<u16>() || &type == &TypeOf<i8>() ||
                &type == &TypeOf<u8>())
            {
                return u8"a number";
            }
            return StringView();
        }

        /// One field of a structure element, checked and ready to write.
        struct FieldWrite
        {
            const PropertyInfo* property = nullptr;
            Variant leaf;       // a leaf's value
            i64 enumerator = 0; // an enum's value
            Guid id;            // an entity or asset reference's target (nil clears)
            u8 kind = 0;        // 0 leaf, 1 enum, 2 entity reference, 3 asset reference
        };

        Result<Function<void()>, String> ShapeStructListWrite(SceneEditContext& edit, const Guid& entity,
                                                              const TypeInfo& componentType,
                                                              const PropertyInfo& list, const JsonValue& value,
                                                              StringView name);

        /// component_set on a LIST: every element's shape checked first (a reference's asset guid or
        /// null, an entity reference's guid or null, a leaf's shape, a structure's fields), then one
        /// write replacing the whole list through MutateComponent (one undo step).
        Result<Function<void()>, String> ShapeListWrite(SceneEditContext& edit, const Guid& entity,
                                                       const TypeInfo& componentType,
                                                       const PropertyInfo& list, const JsonValue& value,
                                                       StringView name)
        {
            const ContainerInfo& ci = *list.type->container;
            const TypeInfo* element = ci.elementType;
            if (element == nullptr)
            {
                return Err(Format(u8"list '{}' has no reflected element type", name));
            }
            const bool reference = IsReferenceType(*element) && element->reference != nullptr;
            const bool entityRef = element == &TypeOf<scene::EntityRef>();
            if (!reference && !entityRef && element->enumeratorCount == 0 && element->propertyCount > 0)
            {
                return ShapeStructListWrite(edit, entity, componentType, list, value, name);
            }
            if (!value.IsArray())
            {
                return Err(Format(u8"property '{}' is a list - `value` is an array of its elements",
                                  name));
            }
            Array<Guid> ids;
            Array<Variant> leaves;
            const usize count = static_cast<usize>(value.Count());
            for (usize i = 0; i < count; ++i)
            {
                const JsonValue item = value.At(static_cast<i64>(i));
                if (reference || entityRef)
                {
                    Guid target;
                    if (!item.IsNull() &&
                        (!item.IsString() || !Guid::TryParse(item.AsString().AsView(), target)))
                    {
                        return Err(Format(u8"element {} of '{}' is not {} guid or null", i, name,
                                          reference ? StringView(u8"an asset") : StringView(u8"an entity")));
                    }
                    ids.PushBack(target);
                }
                else
                {
                    Variant leaf = LeafVariant(*element, item);
                    if (leaf.IsEmpty())
                    {
                        const StringView shape = LeafShape(*element);
                        return Err(Format(u8"element {} of '{}' has the wrong shape{}{}", i, name,
                                          shape.IsEmpty() ? StringView{} : StringView(u8" - it takes "),
                                          shape));
                    }
                    leaves.PushBack(Move(leaf));
                }
            }
            SceneEditContext* editPtr = &edit;
            const TypeInfo* typePtr = &componentType;
            const PropertyInfo* listPtr = &list;
            return Function<void()>{[editPtr, entity, typePtr, listPtr, count, reference, entityRef,
                                     ids = Move(ids), leaves = Move(leaves)]()
            {
                (void)editPtr->MutateComponent(
                    entity, typePtr,
                    [&](const Instance& component)
                    {
                        const Instance container(listPtr->address(component), listPtr->type);
                        const ContainerInfo& c = *listPtr->type->container;
                        while (ContainerSize(c, container) > count)
                        {
                            (void)ContainerRemoveAt(c, container, ContainerSize(c, container) - 1);
                        }
                        while (ContainerSize(c, container) < count)
                        {
                            (void)ContainerEmplaceDefault(c, container, ContainerSize(c, container));
                        }
                        for (usize i = 0; i < count; ++i)
                        {
                            if (reference || entityRef)
                            {
                                const Instance at = ContainerAddressAt(c, container, i);
                                if (at.Pointer() == nullptr)
                                {
                                    continue;
                                }
                                if (reference)
                                {
                                    c.elementType->reference->SetId(at.Pointer(), ids[i]);
                                    c.elementType->reference->ClearBinding(at.Pointer());
                                }
                                else
                                {
                                    static_cast<scene::EntityRef*>(at.Pointer())->id = ids[i];
                                }
                            }
                            else
                            {
                                (void)ContainerSetAt(c, container, i, leaves[i]);
                            }
                        }
                    });
            }};
        }

        bool EnumValueOf(const TypeInfo& type, const JsonValue& value, i64& out);
        String EnumeratorNames(const TypeInfo& type);

        /// A list of structures (a vegetation layer, an IK leg): each element an object naming the
        /// fields it sets, the rest at the element's defaults. A field is a leaf, an enumerator (by
        /// name or number), an entity reference or an asset reference (a guid or null); a list or
        /// structure inside an element is refused (scene_write edits the source).
        Result<Function<void()>, String> ShapeStructListWrite(SceneEditContext& edit, const Guid& entity,
                                                              const TypeInfo& componentType,
                                                              const PropertyInfo& list, const JsonValue& value,
                                                              StringView name)
        {
            const TypeInfo& element = *list.type->container->elementType;
            if (!value.IsArray())
            {
                return Err(Format(u8"property '{}' is a list of structures - `value` is an array of objects, "
                                  u8"each naming the fields it sets",
                                  name));
            }
            const usize count = static_cast<usize>(value.Count());
            Array<Array<FieldWrite>> elements;
            for (usize i = 0; i < count; ++i)
            {
                const JsonValue item = value.At(static_cast<i64>(i));
                if (!item.IsObject())
                {
                    return Err(Format(u8"element {} of '{}' is not an object of its fields", i, name));
                }
                Array<FieldWrite> fields;
                for (const String& key : item.Keys())
                {
                    const PropertyInfo* p = FindProperty(element, reinterpret_cast<const char*>(key.CStr()));
                    if (p == nullptr || p->address == nullptr)
                    {
                        String known;
                        for (const PropertyInfo& q : Properties(element))
                        {
                            known += Format(u8"{}{}", known.IsEmpty() ? StringView{} : StringView(u8", "),
                                            StringView(reinterpret_cast<const utf8char*>(q.name)))
                                         .AsView();
                        }
                        return Err(Format(u8"element {} of '{}' has no field '{}' (its fields: {})", i, name,
                                          key.AsView(), known.AsView()));
                    }
                    const JsonValue field = item.Get(key);
                    FieldWrite w;
                    w.property = p;
                    if (p->type->enumeratorCount > 0)
                    {
                        w.kind = 1;
                        if (!EnumValueOf(*p->type, field, w.enumerator))
                        {
                            return Err(Format(u8"element {} of '{}': '{}' takes one of {}", i, name, key.AsView(),
                                              EnumeratorNames(*p->type).AsView()));
                        }
                    }
                    else if (p->type == &TypeOf<scene::EntityRef>() ||
                             (IsReferenceType(*p->type) && p->type->reference != nullptr))
                    {
                        w.kind = p->type == &TypeOf<scene::EntityRef>() ? 2 : 3;
                        if (!field.IsNull() &&
                            (!field.IsString() || !Guid::TryParse(field.AsString().AsView(), w.id)))
                        {
                            return Err(Format(u8"element {} of '{}': '{}' takes a guid or null", i, name,
                                              key.AsView()));
                        }
                    }
                    else
                    {
                        w.leaf = LeafVariant(*p->type, field);
                        if (w.leaf.IsEmpty())
                        {
                            const StringView shape = LeafShape(*p->type);
                            return Err(Format(u8"element {} of '{}': '{}' {}{}", i, name, key.AsView(),
                                              shape.IsEmpty() ? StringView(u8"is not writable here (scene_write edits "
                                                                           u8"the source)")
                                                              : StringView(u8"takes "),
                                              shape));
                        }
                    }
                    fields.PushBack(Move(w));
                }
                elements.PushBack(Move(fields));
            }
            SceneEditContext* editPtr = &edit;
            const TypeInfo* typePtr = &componentType;
            const PropertyInfo* listPtr = &list;
            return Function<void()>{[editPtr, entity, typePtr, listPtr, count, elements = Move(elements)]()
            {
                (void)editPtr->MutateComponent(
                    entity, typePtr,
                    [&](const Instance& component)
                    {
                        const Instance container(listPtr->address(component), listPtr->type);
                        const ContainerInfo& c = *listPtr->type->container;
                        // Replaced whole: every element starts from its defaults.
                        while (ContainerSize(c, container) > 0)
                        {
                            (void)ContainerRemoveAt(c, container, ContainerSize(c, container) - 1);
                        }
                        for (usize i = 0; i < count; ++i)
                        {
                            (void)ContainerEmplaceDefault(c, container, i);
                            const Instance at = ContainerAddressAt(c, container, i);
                            if (at.Pointer() == nullptr)
                            {
                                continue;
                            }
                            for (const FieldWrite& w : elements[i])
                            {
                                void* address = w.property->address(at);
                                if (address == nullptr)
                                {
                                    continue;
                                }
                                if (w.kind == 0)
                                {
                                    (void)SetProperty(*w.property, at, w.leaf);
                                }
                                else if (w.kind == 1)
                                {
                                    WriteEnumValue(address, *w.property->type, w.enumerator);
                                }
                                else if (w.kind == 2)
                                {
                                    static_cast<scene::EntityRef*>(address)->id = w.id;
                                }
                                else
                                {
                                    w.property->type->reference->SetId(address, w.id);
                                    w.property->type->reference->ClearBinding(address);
                                }
                            }
                        }
                    });
            }};
        }

        /// An enumerator by name or by number; false when neither names one.
        bool EnumValueOf(const TypeInfo& type, const JsonValue& value, i64& out)
        {
            for (u32 i = 0; i < type.enumeratorCount; ++i)
            {
                const EnumValue& enumerator = type.enumerators[i];
                if ((value.IsString() &&
                     value.AsString().AsView() ==
                         StringView(reinterpret_cast<const utf8char*>(enumerator.name))) ||
                    (value.IsNumber() && static_cast<i64>(value.AsNumber()) == enumerator.value))
                {
                    out = enumerator.value;
                    return true;
                }
            }
            return false;
        }

        String EnumeratorNames(const TypeInfo& type)
        {
            String names;
            for (u32 i = 0; i < type.enumeratorCount; ++i)
            {
                if (i > 0)
                {
                    names += u8", ";
                }
                names += StringView(reinterpret_cast<const utf8char*>(type.enumerators[i].name));
            }
            return names;
        }

        constexpr StringView kPageArgument =
            u8"the scene or prefab page's asset guid (default: the active page)";

        // === the viewport: its camera, and a capture of what it shows ===

        JsonValue CameraJson(const AddressedPage& addressed, const EditorCamera& camera)
        {
            JsonValue out = JsonValue::MakeObject();
            out.Set(u8"page", PageJson(*addressed.page));
            out.Set(u8"position", Float3Json(camera.position));
            out.Set(u8"yawDegrees", JsonValue::MakeNumber(static_cast<f64>(RadiansToDegrees(camera.yaw))));
            out.Set(u8"pitchDegrees",
                    JsonValue::MakeNumber(static_cast<f64>(RadiansToDegrees(camera.pitch))));
            out.Set(u8"forward", Float3Json(camera.Forward()));
            out.Set(u8"focusDistance", JsonValue::MakeNumber(static_cast<f64>(camera.focusDistance)));
            return out;
        }

        /// A page title as a file stem: letters, digits, '-' and '_' kept, the rest '_'.
        String FileStemOf(StringView title)
        {
            String stem;
            for (usize i = 0; i < title.Size(); ++i)
            {
                const utf8char c = title[i];
                const bool keep = (c >= u8'a' && c <= u8'z') || (c >= u8'A' && c <= u8'Z') ||
                                  (c >= u8'0' && c <= u8'9') || c == u8'-' || c == u8'_';
                stem += keep ? c : u8'_';
            }
            return stem.IsEmpty() ? String(u8"page") : stem;
        }

        /// One viewport_screenshot in flight: the tool is re-entered every pump with the same
        /// arguments until the page reports the capture written (or it gives up).
        struct PendingCapture
        {
            EditorPage* page = nullptr;
            String path;
            u32 pumps = 0;
            u32 serial = 0; // per host, so two captures of one page never share a default name
        };
        constexpr u32 kCapturePumpLimit = 600; // frames: ten seconds at 60 Hz, then the tool gives up
    }

    Result<AddressedPage, String> ResolveScenePage(EditorContext& context, const JsonValue& args)
    {
        AddressedPage addressed;
        const JsonValue pageArg = args.Get(u8"page");
        if (pageArg.IsString())
        {
            const String text = pageArg.AsString();
            Guid id;
            if (!Guid::TryParse(text.AsView(), id))
            {
                return Err(Format(u8"invalid page guid '{}'", text.AsView()));
            }
            for (const UniquePtr<EditorPage>& open : context.OpenPages())
            {
                if (open->InstanceId() == id)
                {
                    addressed.page = open.Get();
                    break;
                }
            }
            if (addressed.page == nullptr)
            {
                return Err(Format(u8"no open page for guid '{}' (page_list shows the open ones; "
                                  u8"page_open opens one)",
                                  text.AsView()));
            }
        }
        else
        {
            addressed.page = context.ActivePage();
            if (addressed.page == nullptr)
            {
                return Err(String(u8"no page is active - page_open a scene first, or pass "
                                  u8"`page`"));
            }
        }
        addressed.scene = addressed.page->Service<ISceneEditorPage>();
        if (addressed.scene == nullptr)
        {
            return Err(Format(u8"page '{}' is not a scene or prefab page - pass `page` with a "
                              u8"scene's guid, or page_open one",
                              addressed.page->Title()));
        }
        return addressed;
    }

    JsonValue PageJson(const EditorPage& page)
    {
        JsonValue out = JsonValue::MakeObject();
        out.Set(u8"guid", GuidJson(page.InstanceId()));
        out.Set(u8"title", JsonValue::MakeString(String(page.Title())));
        return out;
    }

    bool ReadFloat3(const JsonValue& value, Float3& out)
    {
        if (!value.IsArray() || value.Count() != 3)
        {
            return false;
        }
        for (i64 i = 0; i < 3; ++i)
        {
            if (!value.At(i).IsNumber())
            {
                return false;
            }
        }
        out = Float3{static_cast<f32>(value.At(0).AsNumber()), static_cast<f32>(value.At(1).AsNumber()),
                     static_cast<f32>(value.At(2).AsNumber())};
        return true;
    }

    Result<Guid, String> ResolveSceneEntity(const AddressedPage& addressed, StringView text)
    {
        scene::Scene& scene = addressed.scene->EditContext().Scene();
        const scene::EntityHandle handle = FindEntity(scene, text);
        if (!scene.IsValid(handle))
        {
            return Err(Format(u8"no entity '{}' in page '{}' (a guid, a name or a slash path; "
                              u8"scene_read shows the scene's entities)",
                              text, addressed.page->Title()));
        }
        return scene.GetEntityId(handle);
    }

    void RegisterSceneLiveTools(foundation::mcp::McpServer& server, EditorContext& context)
    {
        RegisterSceneEditTools(server, context);

        EditorContext* ctx = &context;

        server.RegisterTool(
            u8"selection_get",
            u8"A scene page's entity selection: the entities in order (the first is the primary - "
            u8"the gizmo pivot), each with its name. Defaults to the active page.",
            SchemaBuilder().Str(u8"page", kPageArgument).Build(), ToolAnnotations::ReadOnly(),
            [ctx](const JsonValue& args) -> ToolResult
            {
                Result<AddressedPage, String> addressed = ResolveScenePage(*ctx, args);
                if (!addressed.HasValue())
                {
                    return Err(Move(addressed.Error()));
                }
                return SelectionJson(addressed.Value());
            });

        server.RegisterTool(
            u8"selection_set",
            u8"Select entities on a scene page (an empty list clears): the hierarchy, the "
            u8"inspector and the gizmos follow, so this is also how to SHOW the user which entity "
            u8"is meant. The first guid becomes the primary. Every guid must be an entity of that "
            u8"page's scene. Returns the selection as selection_get does.",
            SchemaBuilder()
                .Str(u8"page", kPageArgument)
                .Arr(u8"entities", u8"string", u8"the entity guids to select, in order", true)
                .Build(),
            ToolAnnotations::Adjusts(),
            [ctx](const JsonValue& args) -> ToolResult
            {
                Result<AddressedPage, String> addressed = ResolveScenePage(*ctx, args);
                if (!addressed.HasValue())
                {
                    return Err(Move(addressed.Error()));
                }
                SceneEditContext& edit = addressed.Value().scene->EditContext();
                const JsonValue list = args.Get(u8"entities");
                Array<Guid> ids;
                for (usize i = 0; i < static_cast<usize>(list.Count()); ++i)
                {
                    const String text = list.At(i).AsString();
                    Guid id;
                    if (!Guid::TryParse(text.AsView(), id))
                    {
                        return Err(Format(u8"invalid entity guid '{}'", text.AsView()));
                    }
                    if (!edit.Scene().IsValid(edit.Resolve(id)))
                    {
                        return Err(Format(u8"no entity with guid '{}' in page '{}' (scene_read shows "
                                          u8"the scene's entities)",
                                          text.AsView(), addressed.Value().page->Title()));
                    }
                    ids.PushBack(id);
                }
                edit.EntitySelection().Set(Span<const Guid>{ids.Data(), ids.Size()});
                return SelectionJson(addressed.Value());
            });

        server.RegisterTool(
            u8"simulate_start",
            u8"Start a scene page's edit-mode Simulate: the live scene runs (physics, systems) from "
            u8"a snapshot that simulate_stop restores; edits are locked meanwhile. A no-op when "
            u8"already simulating. Returns {page, simulating}.",
            SchemaBuilder().Str(u8"page", kPageArgument).Build(), ToolAnnotations::Adjusts(),
            [ctx](const JsonValue& args) -> ToolResult
            {
                Result<AddressedPage, String> addressed = ResolveScenePage(*ctx, args);
                if (!addressed.HasValue())
                {
                    return Err(Move(addressed.Error()));
                }
                addressed.Value().scene->StartSimulation();
                return SimulationJson(addressed.Value());
            });

        server.RegisterTool(
            u8"simulate_stop",
            u8"Stop a scene page's Simulate and restore the scene from its snapshot. A no-op when "
            u8"not simulating. Returns {page, simulating}.",
            SchemaBuilder().Str(u8"page", kPageArgument).Build(), ToolAnnotations::Adjusts(),
            [ctx](const JsonValue& args) -> ToolResult
            {
                Result<AddressedPage, String> addressed = ResolveScenePage(*ctx, args);
                if (!addressed.HasValue())
                {
                    return Err(Move(addressed.Error()));
                }
                addressed.Value().scene->StopSimulation();
                return SimulationJson(addressed.Value());
            });

        server.RegisterTool(
            u8"navigation_bake",
            u8"Bake a scene page's navigation zone, as the inspector's Bake Navigation button does: "
            u8"collect the static Mesh geometry inside the zone's box and write the navmesh into "
            u8"the zone's Navigation Zone asset (the zone component's `zone`; assign one first, "
            u8"with component_set). `entity` names the zone's entity; without it, the scene's only "
            u8"zone. The scene is not changed; cook (asset_cook) for the game to see the new "
            u8"navmesh. Refused while the page simulates. Returns {page, entity, asset, baked, "
            u8"triangles, message}: baked false with the reason when nothing walkable came out.",
            SchemaBuilder()
                .Str(u8"page", kPageArgument)
                .Str(u8"entity", u8"the zone's entity: a guid, a name or a slash path (default: "
                                 u8"the scene's only navigation zone)")
                .Build(),
            ToolAnnotations::Adjusts(),
            [ctx](const JsonValue& args) -> ToolResult
            {
                Result<AddressedPage, String> addressed = ResolveScenePage(*ctx, args);
                if (!addressed.HasValue())
                {
                    return Err(Move(addressed.Error()));
                }
                const AddressedPage& page = addressed.Value();
                if (page.scene->IsSimulating())
                {
                    return Err(Format(u8"page '{}' is simulating - stop it (simulate_stop) "
                                      u8"before baking",
                                      page.page->Title()));
                }
                if (ctx->Project() == nullptr)
                {
                    return Err(String(u8"no project is open"));
                }
                SceneEditContext& edit = page.scene->EditContext();
                scene::Scene& sceneRef = edit.Scene();
                auto* zones =
                    sceneRef.GetSystem<engine::navigation::NavMeshZoneComponentManager>();
                scene::EntityHandle zoneEntity{};
                const String named = args.Get(u8"entity").AsString();
                if (!named.IsEmpty())
                {
                    Result<Guid, String> id = ResolveSceneEntity(page, named.AsView());
                    if (!id.HasValue())
                    {
                        return Err(Move(id.Error()));
                    }
                    zoneEntity = edit.Resolve(id.Value());
                    if (zones == nullptr || zones->Get(zoneEntity) == nullptr)
                    {
                        return Err(Format(u8"'{}' has no navigation zone component",
                                          named.AsView()));
                    }
                }
                else
                {
                    usize count = 0;
                    if (zones != nullptr)
                    {
                        zones->ForEach(
                            [&](engine::navigation::NavMeshZoneComponent&, scene::EntityHandle e)
                            {
                                zoneEntity = e;
                                ++count;
                            });
                    }
                    if (count != 1)
                    {
                        return Err(Format(u8"the scene has {} navigation zones - name one with "
                                          u8"`entity`",
                                          count));
                    }
                }
                engine::navigation::NavMeshZoneComponent* zone = zones->Get(zoneEntity);
                foundation::content::Instance* target =
                    zone->zone.id.IsNil() ? nullptr
                                          : ctx->Project()->SourceDb().GetInstance(zone->zone.id);
                if (target == nullptr)
                {
                    return Err(String(u8"the zone has no Navigation Zone asset - create one "
                                      u8"(asset_create) and set it as the zone component's "
                                      u8"`zone` (component_set) before baking"));
                }
                sceneRef.UpdateTransforms();
                const editor::navigation::BakeResult result = editor::navigation::BakeNavigationZone(
                    sceneRef, zoneEntity, *target, editor::navigation::ParallelBakeEnabled(*ctx));
                JsonValue out = JsonValue::MakeObject();
                out.Set(u8"page", PageJson(*page.page));
                out.Set(u8"entity", JsonValue::MakeString(Format(u8"{}", sceneRef.GetEntityId(zoneEntity))));
                out.Set(u8"asset", JsonValue::MakeString(Format(u8"{}", zone->zone.id)));
                out.Set(u8"baked", JsonValue::MakeBool(result.baked));
                out.Set(u8"triangles", JsonValue::MakeNumber(static_cast<f64>(result.triangleCount)));
                out.Set(u8"message", JsonValue::MakeString(editor::navigation::DescribeBake(result)));
                return out;
            });

        server.RegisterTool(
            u8"entity_inspect",
            u8"An entity of a scene page as the editor's inspector sees it: guid, name, active, "
            u8"parent, children, the local transform, and every component the scene holds for it "
            u8"with its reflected properties (references as asset guids, enums by name, nested "
            u8"structures and lists expanded). Defaults to the page's primary selection. With "
            u8"`pie`, the entity as the running game has it now, in that instance's current scene.",
            SchemaBuilder()
                .Str(u8"page", kPageArgument)
                .Str(u8"pie", u8"instead of a page: a running PIE instance's id (pie_list), "
                              u8"reading the scene that game is in")
                .Str(u8"entity", u8"the entity: a guid, a name or a slash path (default: the "
                                 u8"page's primary selection; with `pie`, required)")
                .Build(),
            ToolAnnotations::ReadOnly(),
            [ctx](const JsonValue& args) -> ToolResult
            {
                if (args.Has(u8"pie"))
                {
                    return InspectPie(*ctx, args);
                }
                Result<AddressedPage, String> addressed = ResolveScenePage(*ctx, args);
                if (!addressed.HasValue())
                {
                    return Err(Move(addressed.Error()));
                }
                SceneEditContext& edit = addressed.Value().scene->EditContext();
                Guid id;
                const JsonValue entityArg = args.Get(u8"entity");
                if (entityArg.IsString())
                {
                    Result<Guid, String> named =
                        ResolveSceneEntity(addressed.Value(), entityArg.AsString().AsView());
                    if (!named.HasValue())
                    {
                        return Err(Move(named.Error()));
                    }
                    id = named.Value();
                }
                else
                {
                    const Guid* primary = edit.EntitySelection().Primary();
                    if (primary == nullptr)
                    {
                        return Err(Format(u8"page '{}' has no selection - pass `entity`, or "
                                          u8"selection_set one first",
                                          addressed.Value().page->Title()));
                    }
                    id = *primary;
                }
                if (!edit.Scene().IsValid(edit.Resolve(id)))
                {
                    utf8char text[37];
                    id.ToChars(text);
                    return Err(Format(u8"no entity with guid '{}' in page '{}' (scene_read shows "
                                      u8"the scene's entities)",
                                      StringView(text, 36), addressed.Value().page->Title()));
                }
                JsonValue out = JsonValue::MakeObject();
                out.Set(u8"page", PageJson(*addressed.Value().page));
                out.Set(u8"entity", EntityJson(edit.Scene(), edit.Resolve(id), ctx->Resources()));
                return out;
            });

        server.RegisterTool(
            u8"component_set",
            u8"Set ONE reflected property of an entity's component on a scene page, through the "
            u8"editor's undo path: one undo step per call, labelled mcp, the page marked dirty, "
            u8"nothing saved (file.save / the page's Save does that). `value` takes the shape "
            u8"entity_inspect shows: numbers, booleans, strings, [x,y,z] vectors, [r,g,b,a] "
            u8"colors (sRGB, as a colour picker shows them), [x,y,z,w] quaternions, an enumerator's name, an asset guid for a "
            u8"reference, an entity guid (or null) for an entity reference, and for a list an "
            u8"array of its elements in those shapes, the whole list (a mesh's materials, an "
            u8"animator's mesh entities). REFUSED while the page simulates, on a read-only "
            u8"property, on a nested structure or a list of them (scene_write edits those), and "
            u8"on a value of the wrong shape - nothing changes then. Returns the property as "
            u8"entity_inspect reads it after the write.",
            SchemaBuilder()
                .Str(u8"page", kPageArgument)
                .Str(u8"entity", u8"the entity: a guid, a name or a slash path (default: the "
                                 u8"page's primary selection)")
                .Str(u8"component", u8"the component, as entity_inspect names it: its `type` "
                                    u8"(\"light\", \"physics.RigidBody\") or `typeName`",
                     true)
                .Str(u8"property", u8"the property's name, as entity_inspect shows it", true)
                .Any(u8"value", u8"the new value, in the shape entity_inspect shows the property")
                .Build(),
            ToolAnnotations::Adjusts(),
            [ctx](const JsonValue& args) -> ToolResult
            {
                Result<AddressedPage, String> addressed = ResolveScenePage(*ctx, args);
                if (!addressed.HasValue())
                {
                    return Err(Move(addressed.Error()));
                }
                ISceneEditorPage& page = *addressed.Value().scene;
                const StringView title = addressed.Value().page->Title();
                if (page.IsSimulating())
                {
                    return Err(Format(u8"page '{}' is simulating - edits are locked until "
                                      u8"simulate_stop",
                                      title));
                }
                SceneEditContext& edit = page.EditContext();
                Guid id;
                const JsonValue entityArg = args.Get(u8"entity");
                if (entityArg.IsString())
                {
                    Result<Guid, String> named =
                        ResolveSceneEntity(addressed.Value(), entityArg.AsString().AsView());
                    if (!named.HasValue())
                    {
                        return Err(Move(named.Error()));
                    }
                    id = named.Value();
                }
                else
                {
                    const Guid* primary = edit.EntitySelection().Primary();
                    if (primary == nullptr)
                    {
                        return Err(Format(u8"page '{}' has no selection - pass `entity`, or "
                                          u8"selection_set one first",
                                          title));
                    }
                    id = *primary;
                }
                const scene::EntityHandle handle = edit.Resolve(id);
                if (!edit.Scene().IsValid(handle))
                {
                    utf8char text[37];
                    id.ToChars(text);
                    return Err(Format(u8"no entity with guid '{}' in page '{}'", StringView(text, 36),
                                      title));
                }
                const String component = args.Get(u8"component").AsString();
                scene::ComponentManagerBase* manager =
                    FindComponentManager(edit.Scene(), handle, component.AsView());
                if (manager == nullptr || manager->ComponentType() == nullptr)
                {
                    return Err(Format(u8"entity '{}' has no reflected component '{}' "
                                      u8"(entity_inspect lists its components)",
                                      edit.Scene().GetEntityName(handle), component.AsView()));
                }
                const TypeInfo& type = *manager->ComponentType();
                const String property = args.Get(u8"property").AsString();
                const PropertyInfo* prop = FindProperty(
                    type, reinterpret_cast<const char*>(property.CStr()));
                if (prop == nullptr || prop->type == nullptr)
                {
                    return Err(Format(u8"component '{}' has no property '{}' (entity_inspect "
                                      u8"lists them)",
                                      component.AsView(), property.AsView()));
                }
                if ((static_cast<u32>(prop->flags) & static_cast<u32>(PropertyFlags::ReadOnly)) != 0)
                {
                    return Err(Format(u8"property '{}' of '{}' is read-only", property.AsView(),
                                      component.AsView()));
                }
                const JsonValue value = args.Get(u8"value");
                if (prop->type->container != nullptr)
                {
                    Result<Function<void()>, String> listWrite =
                        ShapeListWrite(edit, id, type, *prop, value, property.AsView());
                    if (!listWrite.HasValue())
                    {
                        return Err(Move(listWrite.Error()));
                    }
                    EditorCommandStack& listCommands = edit.Commands();
                    const i64 listBefore = listCommands.UndoIndex();
                    listCommands.BeginGroup(u8"mcp");
                    listWrite.Value()();
                    listCommands.EndGroup();
                    listCommands.LockGroup();
                    if (listCommands.UndoIndex() == listBefore)
                    {
                        return Err(Format(u8"list '{}' of '{}' could not be set (the command was "
                                          u8"refused; see log_read)",
                                          property.AsView(), component.AsView()));
                    }
                    JsonValue out = JsonValue::MakeObject();
                    out.Set(u8"page", PageJson(*addressed.Value().page));
                    out.Set(u8"entity", GuidJson(id));
                    out.Set(u8"component",
                            JsonValue::MakeString(String(manager->SerializationTypeId())));
                    out.Set(u8"property", JsonValue::MakeString(property));
                    out.Set(u8"value", PropertyJson(*prop, manager->GetComponentInstance(handle)));
                    out.Set(u8"undoSteps", JsonValue::MakeNumber(1));
                    return out;
                }
                if (IsNested(*prop))
                {
                    return Err(Format(u8"property '{}' of '{}' is a {} - not writable through "
                                      u8"component_set yet (scene_write edits the source)",
                                      property.AsView(), component.AsView(),
                                      prop->type->container != nullptr ? StringView(u8"list")
                                                                       : StringView(u8"structure")));
                }
                const TypeInfo& propType = *prop->type;

                // Shape the value first, so a refusal touches nothing; then ONE command in a
                // locked group labelled mcp - one undo step per call that neither the user's
                // scrub of the same property nor the next call merges into.
                Function<void()> write;
                if (IsReferenceType(propType))
                {
                    Guid target;
                    if (!value.IsNull() &&
                        (!value.IsString() || !Guid::TryParse(value.AsString().AsView(), target)))
                    {
                        return Err(Format(u8"property '{}' is a reference - `value` is an asset "
                                          u8"guid or null",
                                          property.AsView()));
                    }
                    SceneEditContext* editPtr = &edit;
                    EditorContext* context = ctx;
                    const TypeInfo* typePtr = &type;
                    const char* name = prop->name;
                    write = [editPtr, context, id, typePtr, name, target]()
                    {
                        editPtr->SetComponentReference(id, typePtr, ComponentPropertyPath{}, name,
                                                       target, context->Resources());
                    };
                }
                else if (propType.enumeratorCount > 0)
                {
                    i64 enumerator = 0;
                    if (!EnumValueOf(propType, value, enumerator))
                    {
                        return Err(Format(u8"property '{}' takes one of: {}", property.AsView(),
                                          EnumeratorNames(propType).AsView()));
                    }
                    SceneEditContext* editPtr = &edit;
                    const TypeInfo* typePtr = &type;
                    const char* name = prop->name;
                    write = [editPtr, id, typePtr, name, enumerator]()
                    { editPtr->SetComponentPropertyRaw(id, typePtr, name, enumerator); };
                }
                else if (&propType == &TypeOf<scene::EntityRef>())
                {
                    Guid target;
                    if (!value.IsNull() &&
                        (!value.IsString() || !Guid::TryParse(value.AsString().AsView(), target)))
                    {
                        return Err(Format(u8"property '{}' is an entity reference - `value` is an "
                                          u8"entity guid or null",
                                          property.AsView()));
                    }
                    SceneEditContext* editPtr = &edit;
                    const TypeInfo* typePtr = &type;
                    const char* name = prop->name;
                    write = [editPtr, id, typePtr, name, target]()
                    { editPtr->SetComponentEntityRef(id, typePtr, name, target); };
                }
                else
                {
                    Variant leaf = LeafVariant(propType, value);
                    if (leaf.IsEmpty())
                    {
                        const StringView shape = LeafShape(propType);
                        if (shape.IsEmpty())
                        {
                            return Err(Format(u8"property '{}' of '{}' is a {} - not writable "
                                              u8"through component_set yet",
                                              property.AsView(), component.AsView(),
                                              StringView(reinterpret_cast<const utf8char*>(propType.name))));
                        }
                        return Err(Format(u8"property '{}' of '{}' takes {} - `value` has the "
                                          u8"wrong shape (entity_inspect shows the current value)",
                                          property.AsView(), component.AsView(), shape));
                    }
                    SceneEditContext* editPtr = &edit;
                    const TypeInfo* typePtr = &type;
                    const char* name = prop->name;
                    write = [editPtr, id, typePtr, name, leaf = Move(leaf)]()
                    { editPtr->SetComponentProperty(id, typePtr, name, leaf); };
                }
                EditorCommandStack& commands = edit.Commands();
                const i64 before = commands.UndoIndex();
                commands.BeginGroup(u8"mcp");
                write();
                commands.EndGroup();
                commands.LockGroup();
                if (commands.UndoIndex() == before)
                {
                    return Err(Format(u8"property '{}' of '{}' could not be set (the command was "
                                      u8"refused; see log_read)",
                                      property.AsView(), component.AsView()));
                }
                const Instance instance = manager->GetComponentInstance(handle);
                JsonValue out = JsonValue::MakeObject();
                out.Set(u8"page", PageJson(*addressed.Value().page));
                out.Set(u8"entity", GuidJson(id));
                out.Set(u8"component", JsonValue::MakeString(String(manager->SerializationTypeId())));
                out.Set(u8"property", JsonValue::MakeString(property));
                out.Set(u8"value", PropertyJson(*prop, instance));
                out.Set(u8"undoSteps", JsonValue::MakeNumber(1));
                return out;
            });
        server.RegisterTool(
            u8"viewport_camera_get",
            u8"The pose a scene page's viewport looks from: the editor camera's position, yaw and "
            u8"pitch in degrees (yaw 0 looks down -Z, positive pitch looks up), its forward vector "
            u8"and its orbit focus distance. Defaults to the active page.",
            SchemaBuilder().Str(u8"page", kPageArgument).Build(), ToolAnnotations::ReadOnly(),
            [ctx](const JsonValue& args) -> ToolResult
            {
                Result<AddressedPage, String> addressed = ResolveScenePage(*ctx, args);
                if (!addressed.HasValue())
                {
                    return Err(Move(addressed.Error()));
                }
                const EditorCamera* camera = addressed.Value().scene->ViewportCamera();
                if (camera == nullptr)
                {
                    return Err(Format(u8"page '{}' has no viewport", addressed.Value().page->Title()));
                }
                return CameraJson(addressed.Value(), *camera);
            });

        server.RegisterTool(
            u8"viewport_camera_set",
            u8"Move a scene page's viewport camera - to look at something from somewhere specific "
            u8"before a viewport_screenshot, or to show the user a spot. Sets what is given: "
            u8"`position` ([x, y, z]), then `yawDegrees` / `pitchDegrees`, then `lookAt` "
            u8"([x, y, z]: aims from the position at that point, horizon level, and moves the "
            u8"orbit focus there - it wins over yaw and pitch). Nothing given changes nothing. "
            u8"Returns the pose as viewport_camera_get does. Editor state only: no scene edit, "
            u8"no undo step.",
            SchemaBuilder()
                .Str(u8"page", kPageArgument)
                .Arr(u8"position", u8"number", u8"the camera position [x, y, z]")
                .Number(u8"yawDegrees", u8"rotation about the up axis; 0 looks down -Z")
                .Number(u8"pitchDegrees", u8"tilt; positive looks up, clamped short of straight up or down")
                .Arr(u8"lookAt", u8"number", u8"the point [x, y, z] to aim at from the position")
                .Build(),
            ToolAnnotations::Adjusts(),
            [ctx](const JsonValue& args) -> ToolResult
            {
                Result<AddressedPage, String> addressed = ResolveScenePage(*ctx, args);
                if (!addressed.HasValue())
                {
                    return Err(Move(addressed.Error()));
                }
                EditorCamera* camera = addressed.Value().scene->ViewportCamera();
                if (camera == nullptr)
                {
                    return Err(Format(u8"page '{}' has no viewport", addressed.Value().page->Title()));
                }
                // Shape everything first: a refusal changes nothing.
                Float3 position;
                Float3 lookAt;
                const bool hasPosition = args.Has(u8"position");
                const bool hasLookAt = args.Has(u8"lookAt");
                if (hasPosition && !ReadFloat3(args.Get(u8"position"), position))
                {
                    return Err(String(u8"`position` takes [x, y, z]"));
                }
                if (hasLookAt && !ReadFloat3(args.Get(u8"lookAt"), lookAt))
                {
                    return Err(String(u8"`lookAt` takes [x, y, z]"));
                }
                if ((args.Has(u8"yawDegrees") && !args.Get(u8"yawDegrees").IsNumber()) ||
                    (args.Has(u8"pitchDegrees") && !args.Get(u8"pitchDegrees").IsNumber()))
                {
                    return Err(String(u8"`yawDegrees` and `pitchDegrees` take a number"));
                }
                if (hasPosition)
                {
                    camera->position = position;
                }
                if (args.Has(u8"yawDegrees"))
                {
                    camera->yaw = DegreesToRadians(static_cast<f32>(args.Get(u8"yawDegrees").AsNumber()));
                }
                if (args.Has(u8"pitchDegrees"))
                {
                    // Short of the poles, as the mouse look is, so the up vector stays defined.
                    const f32 limit = DegreesToRadians(89.0f);
                    camera->pitch = Clamp(
                        DegreesToRadians(static_cast<f32>(args.Get(u8"pitchDegrees").AsNumber())), -limit,
                        limit);
                }
                if (hasLookAt)
                {
                    camera->LookAt(lookAt);
                }
                return CameraJson(addressed.Value(), *camera);
            });

        server.RegisterTool(
            u8"viewport_frame",
            u8"Frame entities in a scene page's viewport, as the user's F (Scene > Frame Selection) "
            u8"does: the editor camera stands back along its view until they all fit, and orbits "
            u8"about their centre. `entities` names them (guids, names or slash paths; default: the "
            u8"selection). What is framed is the box the scene measures each entity and everything "
            u8"under it as (meshes, colliders), or a small box at an entity with nothing to measure. "
            u8"Moves at once, "
            u8"so a viewport_screenshot after it shows the framed view; returns the camera as "
            u8"viewport_camera_get does.",
            SchemaBuilder()
                .Str(u8"page", kPageArgument)
                .Arr(u8"entities", u8"string", u8"the entities to frame (default: the selection)")
                .Build(),
            ToolAnnotations::Adjusts(),
            [ctx](const JsonValue& args) -> ToolResult
            {
                Result<AddressedPage, String> addressed = ResolveScenePage(*ctx, args);
                if (!addressed.HasValue())
                {
                    return Err(Move(addressed.Error()));
                }
                ISceneEditorPage* scene = addressed.Value().scene;
                EditorCamera* camera = scene->ViewportCamera();
                if (camera == nullptr)
                {
                    return Err(Format(u8"page '{}' has no viewport", addressed.Value().page->Title()));
                }
                Array<Guid> ids(ctx->Allocator());
                const JsonValue named = args.Get(u8"entities");
                if (named.IsArray())
                {
                    for (i64 i = 0; i < named.Count(); ++i)
                    {
                        const String text = named.At(i).AsString();
                        Result<Guid, String> id = ResolveSceneEntity(addressed.Value(), text.AsView());
                        if (!id.HasValue())
                        {
                            return Err(Move(id.Error()));
                        }
                        ids.PushBack(id.Value());
                    }
                }
                else
                {
                    for (const Guid& id : scene->EditContext().EntitySelection().Items())
                    {
                        ids.PushBack(id);
                    }
                }
                if (ids.IsEmpty())
                {
                    return Err(String(u8"nothing to frame: pass `entities`, or select some first "
                                      u8"(selection_set)"));
                }
                if (!scene->FrameEntities(Span<const Guid>(ids.Data(), ids.Size()), /*ease*/ false))
                {
                    return Err(Format(u8"page '{}' could not frame those entities",
                                      addressed.Value().page->Title()));
                }
                return CameraJson(addressed.Value(), *camera);
            });

        auto pending = MakeUnique<PendingCapture>(context.Allocator());
        PendingCapture* pendingPtr = pending.Get();
        server.RegisterTool(
            u8"viewport_screenshot",
            u8"What a scene page's viewport shows, as a PNG file: the view as the user sees it at the "
            u8"viewport's size - the scene from the editor camera (viewport_camera_set moves it), "
            u8"with the grid, the gizmo and markers of the selection and the tool's overlay text. "
            u8"Brings the page to front (a hidden viewport never renders), waits for the next frame "
            u8"and the GPU, then returns {page, path, width, height}; read the file. `path` is where "
            u8"to write (an existing directory; default: <user-data>/screenshots/<page>-<pid>-<n>.png). "
            u8"Gives up after ten seconds without a rendered frame.",
            SchemaBuilder()
                .Str(u8"page", kPageArgument)
                .Str(u8"path", u8"the PNG to write (default: a new file under <user-data>/screenshots)")
                .Build(),
            ToolAnnotations::Creates(),
            [ctx, pendingPtr, keep = Move(pending)](const JsonValue& args) -> foundation::mcp::ToolOutcome
            {
                Result<AddressedPage, String> addressed = ResolveScenePage(*ctx, args);
                if (!addressed.HasValue())
                {
                    return Err(Move(addressed.Error()));
                }
                ISceneEditorPage* scene = addressed.Value().scene;
                EditorPage* page = addressed.Value().page;
                if (pendingPtr->page == page)
                {
                    // Re-entered: the same call, one pump later.
                    const ViewportCapture& capture = scene->LastViewportCapture();
                    ++pendingPtr->pumps;
                    if (capture.state == ViewportCaptureState::Written)
                    {
                        JsonValue out = JsonValue::MakeObject();
                        out.Set(u8"page", PageJson(*page));
                        out.Set(u8"path", JsonValue::MakeString(capture.path));
                        out.Set(u8"width", JsonValue::MakeNumber(static_cast<f64>(capture.width)));
                        out.Set(u8"height", JsonValue::MakeNumber(static_cast<f64>(capture.height)));
                        pendingPtr->page = nullptr;
                        return out;
                    }
                    if (capture.state == ViewportCaptureState::Failed)
                    {
                        pendingPtr->page = nullptr;
                        return Err(Format(u8"the capture of page '{}' failed (log_read, category "
                                          u8"Screenshot, says why)",
                                          page->Title()));
                    }
                    if (pendingPtr->pumps > kCapturePumpLimit)
                    {
                        pendingPtr->page = nullptr;
                        return Err(Format(u8"page '{}' rendered no frame in ten seconds - is its "
                                          u8"viewport visible (an editor window minimised or hidden)?",
                                          page->Title()));
                    }
                    return foundation::mcp::ToolOutcome::NotFinished();
                }
                if (scene->ViewportCamera() == nullptr)
                {
                    return Err(Format(u8"page '{}' has no viewport", page->Title()));
                }
                String path = args.Get(u8"path").AsString();
                if (path.IsEmpty())
                {
                    const String dir = PathJoin(GetUserDataDirectory().AsView(), u8"screenshots");
                    if (!CreateDirectories(dir.AsView()))
                    {
                        return Err(Format(u8"could not create '{}'", dir.AsView()));
                    }
                    ++pendingPtr->serial;
                    path = PathJoin(dir.AsView(), Format(u8"{}-{}-{}.png", FileStemOf(page->Title()).AsView(),
                                                         ProcessId(), pendingPtr->serial)
                                                      .AsView());
                }
                ctx->RevealPage(page); // to front: a background tab's viewport never renders
                const Status requested = scene->RequestViewportCapture(path.AsView());
                if (!requested.IsOk())
                {
                    return Err(Format(u8"page '{}' has no viewport", page->Title()));
                }
                pendingPtr->page = page;
                pendingPtr->path = Move(path);
                pendingPtr->pumps = 0;
                return foundation::mcp::ToolOutcome::NotFinished();
            });

    }
}
