// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Mcp - :schema partition
//
// The JSON Schema SUBSET the MCP layer emits (in tools/list inputSchema) and validates (on
// tools/call). Deliberately small - object/string/number/integer/boolean/array with properties,
// required, items, enum, description, default - built through a fluent API (no schema strings by
// hand, no general JSON-Schema engine). Validation names the offending field so the caller can
// return a JSON-RPC -32602 that an agent can act on.

module;
#include "Core/Prelude.h"

export module foundation.mcp:schema;

import foundation.core;
import foundation.json;

using namespace foundation::core;
using foundation::json::JsonValue;

export namespace foundation::mcp
{
    // Fluent builder for an object schema. Each Field adds to `properties`; `required` marks it.
    class SchemaBuilder
    {
        JsonValue m_properties = JsonValue::MakeObject();
        JsonValue m_required = JsonValue::MakeArray();
        bool m_anyFields = false;

    public:
        SchemaBuilder& Str(String name, String description = {}, bool required = false)
        {
            return Scalar(Move(name), u8"string", Move(description), required);
        }
        SchemaBuilder& Number(String name, String description = {}, bool required = false)
        {
            return Scalar(Move(name), u8"number", Move(description), required);
        }
        SchemaBuilder& Integer(String name, String description = {}, bool required = false)
        {
            return Scalar(Move(name), u8"integer", Move(description), required);
        }
        SchemaBuilder& Boolean(String name, String description = {}, bool required = false)
        {
            return Scalar(Move(name), u8"boolean", Move(description), required);
        }

        // A string field constrained to a fixed set of values.
        SchemaBuilder& Enum(String name, Array<String> values, String description = {},
                            bool required = false)
        {
            JsonValue prop = JsonValue::MakeObject();
            prop.Set(u8"type", JsonValue::MakeString(u8"string"));
            if (!description.IsEmpty())
            {
                prop.Set(u8"description", JsonValue::MakeString(Move(description)));
            }
            JsonValue choices = JsonValue::MakeArray();
            for (const auto& v : values)
            {
                choices.Add(JsonValue::MakeString(v));
            }
            prop.Set(u8"enum", Move(choices));
            return Add(Move(name), Move(prop), required);
        }

        // An array field whose elements are a scalar type ("string"/"number"/...).
        SchemaBuilder& Arr(String name, String itemType, String description = {},
                           bool required = false)
        {
            JsonValue prop = JsonValue::MakeObject();
            prop.Set(u8"type", JsonValue::MakeString(u8"array"));
            if (!description.IsEmpty())
            {
                prop.Set(u8"description", JsonValue::MakeString(Move(description)));
            }
            JsonValue items = JsonValue::MakeObject();
            items.Set(u8"type", JsonValue::MakeString(Move(itemType)));
            prop.Set(u8"items", Move(items));
            return Add(Move(name), Move(prop), required);
        }

        // A nested object whose fields `fields` declares: checked as the arguments are (its required
        // fields, the type of each, and a field it does not declare refused, named by its path).
        SchemaBuilder& Obj(String name, const SchemaBuilder& fields, String description = {},
                           bool required = false)
        {
            JsonValue prop = fields.Build();
            if (!description.IsEmpty())
            {
                prop.Set(u8"description", JsonValue::MakeString(Move(description)));
            }
            return Add(Move(name), Move(prop), required);
        }

        // An array of objects, each checked against `fields` as Obj checks one.
        SchemaBuilder& ObjectArr(String name, const SchemaBuilder& fields, String description = {},
                                 bool required = false)
        {
            JsonValue prop = JsonValue::MakeObject();
            prop.Set(u8"type", JsonValue::MakeString(u8"array"));
            if (!description.IsEmpty())
            {
                prop.Set(u8"description", JsonValue::MakeString(Move(description)));
            }
            prop.Set(u8"items", fields.Build());
            return Add(Move(name), Move(prop), required);
        }

        // A map of names the tool resolves itself (an importer's options, a behaviour's properties):
        // an object that takes any key (additionalProperties:true). The tool refuses a key it does
        // not know, with its own list.
        SchemaBuilder& Map(String name, String description = {}, bool required = false)
        {
            JsonValue prop = JsonValue::MakeObject();
            prop.Set(u8"type", JsonValue::MakeString(u8"object"));
            if (!description.IsEmpty())
            {
                prop.Set(u8"description", JsonValue::MakeString(Move(description)));
            }
            prop.Set(u8"additionalProperties", JsonValue::MakeBool(true));
            return Add(Move(name), Move(prop), required);
        }

        // A field of any JSON shape (no type constraint): declared, so the call may pass it, and
        // described, so an agent knows what goes there.
        SchemaBuilder& Any(String name, String description = {}, bool required = false)
        {
            JsonValue prop = JsonValue::MakeObject();
            if (!description.IsEmpty())
            {
                prop.Set(u8"description", JsonValue::MakeString(Move(description)));
            }
            return Add(Move(name), Move(prop), required);
        }

        // The tool takes fields beyond the declared ones (additionalProperties:true): the server
        // passes them through unchecked instead of refusing them.
        SchemaBuilder& AnyFields()
        {
            m_anyFields = true;
            return *this;
        }

        // Escape hatch: a fully-formed property schema (nested objects/arrays).
        SchemaBuilder& Property(String name, JsonValue propSchema, bool required = false)
        {
            return Add(Move(name), Move(propSchema), required);
        }

        // The finished object schema: {type:"object", properties:{...}, required:[...]}.
        [[nodiscard]] JsonValue Build() const
        {
            JsonValue schema = JsonValue::MakeObject();
            schema.Set(u8"type", JsonValue::MakeString(u8"object"));
            schema.Set(u8"properties", m_properties);
            schema.Set(u8"required", m_required);
            if (m_anyFields)
            {
                schema.Set(u8"additionalProperties", JsonValue::MakeBool(true));
            }
            return schema;
        }

    private:
        SchemaBuilder& Scalar(String name, const char8_t* type, String description, bool required)
        {
            JsonValue prop = JsonValue::MakeObject();
            prop.Set(u8"type", JsonValue::MakeString(type));
            if (!description.IsEmpty())
            {
                prop.Set(u8"description", JsonValue::MakeString(Move(description)));
            }
            return Add(Move(name), Move(prop), required);
        }
        SchemaBuilder& Add(String name, JsonValue prop, bool required)
        {
            if (required)
            {
                m_required.Add(JsonValue::MakeString(name));
            }
            m_properties.Set(Move(name), Move(prop));
            return *this;
        }
    };

    // A JsonValue matches a subset type tag. "integer" accepts any JSON number (the wire has no
    // separate integer type); everything else is exact.
    [[nodiscard]] inline bool JsonMatchesType(const JsonValue& v, StringView type)
    {
        if (type == StringView(u8"string"))
        {
            return v.IsString();
        }
        if (type == StringView(u8"number") || type == StringView(u8"integer"))
        {
            return v.IsNumber();
        }
        if (type == StringView(u8"boolean"))
        {
            return v.IsBool();
        }
        if (type == StringView(u8"array"))
        {
            return v.IsArray();
        }
        if (type == StringView(u8"object"))
        {
            return v.IsObject();
        }
        return true; // unknown/absent type constraint: accept
    }

    namespace schema_detail
    {
        [[nodiscard]] inline String Join(const String& path, StringView key)
        {
            if (path.IsEmpty())
            {
                return String(key);
            }
            return Format(u8"{}.{}", path.AsView(), key);
        }

        [[nodiscard]] inline String DeclaredNames(const JsonValue& properties)
        {
            String declared;
            for (i64 j = 0; j < properties.Count(); ++j)
            {
                if (j > 0)
                {
                    declared.Append(StringView(u8", "));
                }
                declared.Append(properties.KeyAt(j).AsView());
            }
            return declared;
        }

        [[nodiscard]] inline Optional<String> ValidateObject(const JsonValue& value, const JsonValue& schema,
                                                             const String& path);

        // One value against its property schema, at `path`: its type, its enum, then what is inside
        // it (an object that declares its properties, an array's elements).
        [[nodiscard]] inline Optional<String> ValidateValue(const JsonValue& value, const JsonValue& prop,
                                                            const String& path)
        {
            const String type = prop.Get(u8"type").AsString();
            if (!JsonMatchesType(value, type.AsView()))
            {
                return Format(u8"field '{}' must be of type {}", path.AsView(), type.AsView());
            }
            const JsonValue choices = prop.Get(u8"enum");
            if (choices.IsArray())
            {
                bool allowed = false;
                for (i64 j = 0; j < choices.Count(); ++j)
                {
                    if (choices.At(j).AsString() == value.AsString())
                    {
                        allowed = true;
                        break;
                    }
                }
                if (!allowed)
                {
                    return Format(u8"field '{}' is not one of the allowed values", path.AsView());
                }
            }
            if (value.IsObject() && prop.Get(u8"properties").IsObject())
            {
                return ValidateObject(value, prop, path);
            }
            const JsonValue items = prop.Get(u8"items");
            if (value.IsArray() && items.IsObject())
            {
                for (i64 i = 0; i < value.Count(); ++i)
                {
                    Optional<String> error =
                        ValidateValue(value.At(i), items, Format(u8"{}[{}]", path.AsView(), i));
                    if (error.HasValue())
                    {
                        return error;
                    }
                }
            }
            return {};
        }

        // An object against an object schema: its required fields, then each field it has. A field
        // the schema does not declare is refused, naming the ones it does, unless the schema sets
        // additionalProperties:true. At the top (`path` empty) a field is an "argument".
        [[nodiscard]] inline Optional<String> ValidateObject(const JsonValue& value, const JsonValue& schema,
                                                             const String& path)
        {
            const JsonValue required = schema.Get(u8"required");
            for (i64 i = 0; i < required.Count(); ++i)
            {
                const String key = required.At(i).AsString();
                if (!value.Has(key))
                {
                    return Format(u8"missing required field '{}'", Join(path, key.AsView()).AsView());
                }
            }
            const JsonValue properties = schema.Get(u8"properties");
            const JsonValue open = schema.Get(u8"additionalProperties");
            const bool openEnded = open.IsBool() && open.AsBool();
            for (i64 i = 0; i < value.Count(); ++i)
            {
                const String key = value.KeyAt(i);
                const JsonValue prop = properties.Get(key);
                if (prop.IsNull())
                {
                    if (openEnded)
                    {
                        continue; // any field goes: not validated
                    }
                    const String declared = DeclaredNames(properties);
                    const String where = Join(path, key.AsView());
                    const char8_t* noun = path.IsEmpty() ? u8"argument" : u8"field";
                    return properties.Count() == 0
                               ? Format(u8"no {} '{}' (it takes none)", StringView(noun), where.AsView())
                               : Format(u8"no {} '{}' (it takes: {})", StringView(noun), where.AsView(),
                                        declared.AsView());
                }
                Optional<String> error = ValidateValue(value.Get(key), prop, Join(path, key.AsView()));
                if (error.HasValue())
                {
                    return error;
                }
            }
            return {};
        }
    }

    // Validate `args` against an object `schema`. Returns an empty Optional when valid, else a
    // human-readable message naming the offending field by its path (destined for a -32602
    // response): "no argument 'X' (it takes: ...)", "no field 'probes[0].field' (it takes: ...)",
    // "missing required field 'until.op'", "field 'sampleAt[1]' must be of type number". A field
    // the schema does not declare is refused at any depth, naming the ones it does: a misspelt
    // name would otherwise be dropped without a word and the call run without it. An object that
    // sets additionalProperties:true (a map; see SchemaBuilder::Map, AnyFields) takes any field,
    // and so does one that declares no properties at all (an object of unknown shape).
    [[nodiscard]] inline Optional<String> ValidateArgs(const JsonValue& args, const JsonValue& schema)
    {
        if (!args.IsObject())
        {
            return String(u8"arguments must be a JSON object");
        }
        return schema_detail::ValidateObject(args, schema, String());
    }
}
