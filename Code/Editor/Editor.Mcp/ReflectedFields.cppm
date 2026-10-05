// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Mcp - :reflected_fields partition.
//
// A record's fields over MCP, read from its reflection rather than spelled out per tool: the
// project's settings (project_info / project_settings_set) and an export preset
// (export_presets / export_preset_set) are both labelled properties. The kinds a field may be:
// text, a list of texts, an asset's guid or a list of them (the `assetType` attribute names the
// type), a count (within its `range`), a flag, a choice (an enum, by its values' names). The
// answer, the arguments' schema, the check of given values and their write are each one function
// here, so the tools differ only in their own rules (MSAA's levels, a preset's platform).

module;
#include "Core/Prelude.h"

export module editor.mcp:reflected_fields;

import foundation.core;
import foundation.json;
import foundation.content;
import foundation.mcp;
import engine.project;
import editor.project;
import :session;

using namespace foundation::core;
using foundation::json::JsonValue;
namespace content = foundation::content;

export namespace editor::mcp::detail
{
    inline StringView PropertyName(const PropertyInfo& property)
    {
        return StringView(reinterpret_cast<const utf8char*>(property.name));
    }

    /// A field holding a list of texts.
    [[nodiscard]] inline bool IsTextListField(const PropertyInfo& property) noexcept
    {
        return property.type == &TypeOf<Array<String>>();
    }

    /// The fields of `type` these tools read and set: the labelled properties of a kind above.
    inline Array<const PropertyInfo*> ReflectedFieldsOf(const TypeInfo& type)
    {
        Array<const PropertyInfo*> out;
        for (const PropertyInfo& property : Properties(type))
        {
            const bool kind = property.type == &TypeOf<String>() || IsTextListField(property) ||
                              property.type == &TypeOf<u32>() || property.type == &TypeOf<bool>() ||
                              IsEnum(*property.type) || engine::project::IsAssetSetting(property) ||
                              engine::project::IsAssetListSetting(property);
            if (kind && engine::project::SettingAttribute(property, engine::project::kSettingLabelAttribute) !=
                            nullptr)
            {
                out.PushBack(&property);
            }
        }
        return out;
    }

    /// A count field's bounds: its reflected `range` (min, max), else every u32.
    inline void CountRange(const PropertyInfo& property, u32& least, u32& most)
    {
        least = 0;
        most = 0xFFFFFFFFu;
        if (const Attribute* range = FindAttribute(property, u8"range"))
        {
            if (const Float4* bounds = range->value.TryGet<Float4>())
            {
                least = static_cast<u32>(bounds->x);
                most = static_cast<u32>(bounds->y);
            }
        }
    }

    /// A choice field's values by name, comma separated (what a refusal lists).
    inline String EnumNames(const TypeInfo& type)
    {
        String names;
        for (const EnumValue& value : Enumerators(type))
        {
            names += names.IsEmpty() ? u8"" : u8", ";
            names += StringView(reinterpret_cast<const utf8char*>(value.name));
        }
        return names;
    }

    /// An asset a field names as {guid, path}; the path is null when the guid names nothing (or
    /// there is no project to look in).
    inline JsonValue FieldAssetJson(editor::EditorProject* project, const Guid& id)
    {
        content::Instance* asset = project != nullptr ? project->SourceDb().GetInstance(id) : nullptr;
        JsonValue entry = JsonValue::MakeObject();
        entry.Set(u8"guid", GuidToJson(id));
        entry.Set(u8"path", asset != nullptr ? JsonValue::MakeString(asset->Path()) : JsonValue::MakeNull());
        return entry;
    }

    /// The fields' values: an asset {guid, path} (null when unset), a list as an array, a text as
    /// text, a flag as a bool, a choice by its value's name, a count as a number.
    inline JsonValue FieldsJson(const Instance& record, Span<const PropertyInfo* const> fields,
                                editor::EditorProject* project)
    {
        JsonValue out = JsonValue::MakeObject();
        for (const PropertyInfo* field : fields)
        {
            const String key(PropertyName(*field));
            const void* address = field->address(record);
            if (engine::project::IsAssetListSetting(*field))
            {
                JsonValue list = JsonValue::MakeArray();
                for (const Guid& id : *static_cast<const Array<Guid>*>(address))
                {
                    list.Add(FieldAssetJson(project, id));
                }
                out.Set(key, Move(list));
            }
            else if (engine::project::IsAssetSetting(*field))
            {
                const Guid& id = *static_cast<const Guid*>(address);
                out.Set(key, id.IsNil() ? JsonValue::MakeNull() : FieldAssetJson(project, id));
            }
            else if (IsTextListField(*field))
            {
                JsonValue list = JsonValue::MakeArray();
                for (const String& text : *static_cast<const Array<String>*>(address))
                {
                    list.Add(JsonValue::MakeString(text));
                }
                out.Set(key, Move(list));
            }
            else if (field->type == &TypeOf<String>())
            {
                out.Set(key, JsonValue::MakeString(*static_cast<const String*>(address)));
            }
            else if (field->type == &TypeOf<bool>())
            {
                out.Set(key, JsonValue::MakeBool(*static_cast<const bool*>(address)));
            }
            else if (IsEnum(*field->type))
            {
                const char* name = EnumValueName(*field->type, ReadEnumValue(address, *field->type));
                out.Set(key, name != nullptr
                                 ? JsonValue::MakeString(String(StringView(reinterpret_cast<const utf8char*>(name))))
                                 : JsonValue::MakeNull());
            }
            else
            {
                out.Set(key, JsonValue::MakeNumber(static_cast<f64>(*static_cast<const u32*>(address))));
            }
        }
        return out;
    }

    /// One argument per field, as reflection describes it.
    inline void AddFieldsToSchema(foundation::mcp::SchemaBuilder& schema, Span<const PropertyInfo* const> fields)
    {
        for (const PropertyInfo* field : fields)
        {
            const String* label = engine::project::SettingAttribute(*field, engine::project::kSettingLabelAttribute);
            const String key(PropertyName(*field));
            const String* assetType =
                engine::project::SettingAttribute(*field, engine::project::kSettingAssetTypeAttribute);
            if (engine::project::IsAssetListSetting(*field))
            {
                schema.Arr(key, u8"string",
                           Format(u8"{}: the guids of assets of type {}, in order; the whole list, [] for none",
                                  label->AsView(), assetType->AsView()));
            }
            else if (assetType != nullptr)
            {
                const String* emptyText =
                    engine::project::SettingAttribute(*field, engine::project::kSettingEmptyTextAttribute);
                schema.Str(key, Format(u8"{}: the guid of an asset of type {}; \"\" clears it ({})", label->AsView(),
                                       assetType->AsView(), emptyText != nullptr ? emptyText->AsView() : StringView()));
            }
            else if (IsTextListField(*field))
            {
                schema.Arr(key, u8"string", Format(u8"{}: the whole list, [] for none", label->AsView()));
            }
            else if (field->type == &TypeOf<String>())
            {
                schema.Str(key, *label);
            }
            else if (field->type == &TypeOf<bool>())
            {
                schema.Boolean(key, *label);
            }
            else if (IsEnum(*field->type))
            {
                Array<String> names;
                for (const EnumValue& value : Enumerators(*field->type))
                {
                    names.PushBack(String(StringView(reinterpret_cast<const utf8char*>(value.name))));
                }
                schema.Enum(key, Move(names), *label);
            }
            else
            {
                u32 least = 0;
                u32 most = 0;
                CountRange(*field, least, most);
                schema.Integer(key, most == 0xFFFFFFFFu ? *label
                                                        : Format(u8"{}: {} to {}", label->AsView(), least, most));
            }
        }
    }

    /// One given value, checked and held until every one is (a refusal leaves the record as it
    /// was).
    struct FieldChange
    {
        const PropertyInfo* field = nullptr;
        Guid id;
        Array<Guid> ids;    // an asset list's whole list
        Array<String> texts; // a text list's whole list
        bool flag = false;
        i64 choice = 0;
        String text;
        u32 number = 0;
    };

    /// Checks every given argument that names one of `fields` against its kind: an asset must
    /// exist in `project` and be of the field's type (an asset list's entries once each), a count
    /// within its range, a choice one of its values, a flag a bool. The changes, or the refusal.
    inline Result<Array<FieldChange>, String> CheckFields(const JsonValue& args, Span<const PropertyInfo* const> fields,
                                                          editor::EditorProject& project)
    {
        Array<FieldChange> changes;
        for (const PropertyInfo* field : fields)
        {
            const String key(PropertyName(*field));
            if (!args.Has(key))
            {
                continue;
            }
            const JsonValue value = args.Get(key);
            FieldChange change;
            change.field = field;
            const String* assetType =
                engine::project::SettingAttribute(*field, engine::project::kSettingAssetTypeAttribute);
            // One guid naming an asset of the field's type: the refusal, empty when it does;
            // `where` names the field in it.
            const auto checkAsset = [&project, assetType](StringView where, StringView text, Guid& id) -> String
            {
                if (!Guid::TryParse(text, id))
                {
                    return Format(u8"`{}`: '{}' is not a valid guid", where, text);
                }
                content::Instance* asset = project.SourceDb().GetInstance(id);
                if (asset == nullptr)
                {
                    return Format(u8"`{}`: no asset with guid {} in the project", where, text);
                }
                if (asset->TypeName() != assetType->AsView())
                {
                    return Format(u8"`{}` takes an asset of type {}; '{}' is of type {}", where, assetType->AsView(),
                                  asset->Name(), asset->TypeName());
                }
                return String();
            };
            if (engine::project::IsAssetListSetting(*field))
            {
                if (!value.IsArray())
                {
                    return Err(Format(u8"`{}` takes an array of {} guids", key.AsView(), assetType->AsView()));
                }
                for (i64 i = 0; i < value.Count(); ++i)
                {
                    Guid id;
                    const String text = value.At(i).AsString();
                    String refused = checkAsset(Format(u8"{}[{}]", key.AsView(), i).AsView(), text.AsView(), id);
                    if (!refused.IsEmpty())
                    {
                        return Err(Move(refused));
                    }
                    bool listed = false;
                    for (const Guid& other : change.ids)
                    {
                        listed = listed || other == id;
                    }
                    if (!listed)
                    {
                        change.ids.PushBack(id); // once each
                    }
                }
            }
            else if (assetType != nullptr)
            {
                const String text = value.AsString();
                if (!text.IsEmpty())
                {
                    String refused = checkAsset(key.AsView(), text.AsView(), change.id);
                    if (!refused.IsEmpty())
                    {
                        return Err(Move(refused));
                    }
                }
            }
            else if (IsTextListField(*field))
            {
                if (!value.IsArray())
                {
                    return Err(Format(u8"`{}` takes an array of texts", key.AsView()));
                }
                for (i64 i = 0; i < value.Count(); ++i)
                {
                    change.texts.PushBack(value.At(i).AsString());
                }
            }
            else if (field->type == &TypeOf<String>())
            {
                change.text = value.AsString();
            }
            else if (field->type == &TypeOf<bool>())
            {
                if (!value.IsBool())
                {
                    return Err(Format(u8"`{}` takes true or false", key.AsView()));
                }
                change.flag = value.AsBool();
            }
            else if (IsEnum(*field->type))
            {
                const String name = value.AsString();
                if (!EnumValueByName(*field->type, reinterpret_cast<const char*>(name.CStr()), change.choice))
                {
                    return Err(Format(u8"`{}` takes {}", key.AsView(), EnumNames(*field->type).AsView()));
                }
            }
            else
            {
                u32 least = 0;
                u32 most = 0;
                CountRange(*field, least, most);
                if (!value.IsNumber() || value.AsNumber() < static_cast<f64>(least) ||
                    value.AsNumber() > static_cast<f64>(most))
                {
                    return Err(most == 0xFFFFFFFFu ? Format(u8"`{}` takes a count from {}", key.AsView(), least)
                                                   : Format(u8"`{}` takes {} to {}", key.AsView(), least, most));
                }
                change.number = static_cast<u32>(value.AsInt());
            }
            changes.PushBack(Move(change));
        }
        return changes;
    }

    /// Writes checked changes into `record`.
    inline void ApplyFields(const Instance& record, Span<const FieldChange> changes)
    {
        for (const FieldChange& change : changes)
        {
            void* address = change.field->address(record);
            if (engine::project::IsAssetListSetting(*change.field))
            {
                *static_cast<Array<Guid>*>(address) = change.ids;
            }
            else if (engine::project::IsAssetSetting(*change.field))
            {
                *static_cast<Guid*>(address) = change.id;
            }
            else if (IsTextListField(*change.field))
            {
                *static_cast<Array<String>*>(address) = change.texts;
            }
            else if (change.field->type == &TypeOf<String>())
            {
                *static_cast<String*>(address) = change.text;
            }
            else if (change.field->type == &TypeOf<bool>())
            {
                *static_cast<bool*>(address) = change.flag;
            }
            else if (IsEnum(*change.field->type))
            {
                WriteEnumValue(address, *change.field->type, change.choice);
            }
            else
            {
                *static_cast<u32*>(address) = change.number;
            }
        }
    }
}
