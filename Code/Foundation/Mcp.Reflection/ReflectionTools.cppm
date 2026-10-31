// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Mcp.Reflection - `foundation.mcp.reflection`
//
// The reflection tools, type_list and type_info, registered against a McpServer. They let an
// agent discover the authored surface (types, properties, methods, enums) so it can write correct
// scripts and scene edits. They are pure queries over a TypeRegistry, with no project or pipeline
// dependency. Each module contributes its own tools; these are the reflection module's.

module;
#include "Core/Prelude.h"

export module foundation.mcp.reflection;

import foundation.core;
import foundation.json;
import foundation.mcp;

using namespace foundation::core;
using foundation::json::JsonValue;

namespace foundation::mcp::detail
{
    // A reflection char* name (UTF-8, may be null) as a JSON string.
    inline JsonValue Name(const char* s)
    {
        return s == nullptr ? JsonValue::MakeString(String())
                            : JsonValue::MakeString(String(reinterpret_cast<const char8_t*>(s)));
    }

    inline bool NamespaceStartsWith(const char* ns, StringView prefix)
    {
        if (ns == nullptr)
        {
            return prefix.Size() == 0;
        }
        for (usize i = 0; i < prefix.Size(); ++i)
        {
            if (ns[i] == '\0' ||
                static_cast<char8_t>(static_cast<unsigned char>(ns[i])) != prefix.Data()[i])
            {
                return false;
            }
        }
        return true;
    }

    inline bool NameEquals(const char* a, StringView b)
    {
        if (a == nullptr)
        {
            return b.Size() == 0;
        }
        usize i = 0;
        for (; i < b.Size(); ++i)
        {
            if (a[i] == '\0' ||
                static_cast<char8_t>(static_cast<unsigned char>(a[i])) != b.Data()[i])
            {
                return false;
            }
        }
        return a[i] == '\0'; // exact match: a is fully consumed
    }

    // A domain's readable name from the small known set (it hashes the literals, so there is no
    // dependency on the Pipeline or Editor modules). Empty for a custom domain the tool does not
    // know by name; the in-player bit (below) still works for any domain.
    inline StringView DomainName(TypeDomain domain)
    {
        if (domain == kRuntimeTypeDomain)
        {
            return u8"Runtime";
        }
        if (domain == TypeDomain(u8"Pipeline"))
        {
            return u8"Pipeline";
        }
        if (domain == TypeDomain(u8"Editor"))
        {
            return u8"Editor";
        }
        return u8"";
    }

    // Emit {domain, inPlayer} for a type: `domain` is the availability domain's readable name;
    // `inPlayer` is true iff it ships in a runtime player (the Runtime domain), which is what an
    // agent needs to know (a TextureAsset exists for authoring but not in a runtime player). Any
    // other domain is authoring-only.
    inline void SetDomain(JsonValue& out, const TypeRegistry& reg, TypeId id)
    {
        const TypeDomain domain = reg.DomainOf(id);
        const StringView name = DomainName(domain);
        if (!name.IsEmpty())
        {
            out.Set(u8"domain", JsonValue::MakeString(String(name)));
        }
        out.Set(u8"inPlayer", JsonValue::MakeBool(domain == kRuntimeTypeDomain));
    }

    // The full reflected surface of one type as a JSON object.
    inline JsonValue DescribeType(const TypeInfo& type, const TypeRegistry& reg)
    {
        JsonValue out = JsonValue::MakeObject();
        out.Set(u8"name", Name(type.name));
        out.Set(u8"namespace", Name(type.namespaceName));
        SetDomain(out, reg, type.id);
        if (type.base != nullptr)
        {
            out.Set(u8"base", Name(type.base->name));
        }

        JsonValue properties = JsonValue::MakeArray();
        for (u32 i = 0; i < type.propertyCount; ++i)
        {
            const PropertyInfo& p = type.properties[i];
            JsonValue jp = JsonValue::MakeObject();
            jp.Set(u8"name", Name(p.name));
            jp.Set(u8"type", Name(p.type != nullptr ? p.type->name : nullptr));
            properties.Add(Move(jp));
        }
        out.Set(u8"properties", Move(properties));

        JsonValue methods = JsonValue::MakeArray();
        for (u32 i = 0; i < type.methodCount; ++i)
        {
            const MethodInfo& m = type.methods[i];
            JsonValue jm = JsonValue::MakeObject();
            jm.Set(u8"name", Name(m.name));
            const TypeInfo* returnType = m.returnType != nullptr ? m.returnType() : nullptr;
            jm.Set(u8"returns", returnType != nullptr ? Name(returnType->name)
                                                      : JsonValue::MakeString(u8"void"));
            JsonValue params = JsonValue::MakeArray();
            for (u32 j = 0; j < m.paramCount; ++j)
            {
                const ParamInfo& pi = m.params[j];
                const TypeInfo* paramType = pi.type != nullptr ? pi.type() : nullptr;
                JsonValue jpi = JsonValue::MakeObject();
                jpi.Set(u8"name", Name(pi.name));
                jpi.Set(u8"type", Name(paramType != nullptr ? paramType->name : nullptr));
                params.Add(Move(jpi));
            }
            jm.Set(u8"params", Move(params));
            methods.Add(Move(jm));
        }
        out.Set(u8"methods", Move(methods));

        if (type.enumeratorCount > 0)
        {
            JsonValue values = JsonValue::MakeArray();
            for (u32 i = 0; i < type.enumeratorCount; ++i)
            {
                JsonValue je = JsonValue::MakeObject();
                je.Set(u8"name", Name(type.enumerators[i].name));
                je.Set(u8"value", JsonValue::MakeNumber(static_cast<f64>(type.enumerators[i].value)));
                values.Add(Move(je));
            }
            out.Set(u8"enum", Move(values));
        }
        return out;
    }
}

export namespace foundation::mcp
{
    // Registers type_list and type_info against `server`, querying `registry` live at call time (so
    // types registered after this call are still visible). `registry` must outlive `server`; the
    // default GlobalTypeRegistry is process-lifetime.
    inline void RegisterReflectionTools(McpServer& server,
                                        const core::TypeRegistry& registry = core::GlobalTypeRegistry())
    {
        const core::TypeRegistry* reg = &registry;

        server.RegisterTool(
            u8"type_list",
            u8"List reflected types (name + namespace), optionally filtered by a namespace prefix.",
            SchemaBuilder()
                .Str(u8"namespace", u8"only types whose namespace starts with this prefix")
                .Build(),
                foundation::mcp::ToolAnnotations::ReadOnly(),
            [reg](const JsonValue& args) -> ToolResult
            {
                const String nsFilter = args.Get(u8"namespace").AsString();
                JsonValue types = JsonValue::MakeArray();
                const Array<const TypeInfo*>& all = reg->All();
                for (usize i = 0; i < all.Size(); ++i)
                {
                    const TypeInfo* t = all[i];
                    if (!nsFilter.IsEmpty() &&
                        !detail::NamespaceStartsWith(t->namespaceName, nsFilter.AsView()))
                    {
                        continue;
                    }
                    JsonValue e = JsonValue::MakeObject();
                    e.Set(u8"name", detail::Name(t->name));
                    e.Set(u8"namespace", detail::Name(t->namespaceName));
                    detail::SetDomain(e, *reg, t->id);
                    types.Add(Move(e));
                }
                const i64 count = types.Count();
                JsonValue out = JsonValue::MakeObject();
                out.Set(u8"count", JsonValue::MakeNumber(static_cast<f64>(count)));
                out.Set(u8"types", Move(types));
                return out;
            });

        server.RegisterTool(
            u8"type_info",
            u8"Describe a reflected type: its properties, methods (with params/returns), and enum "
            u8"values.",
            SchemaBuilder()
                .Str(u8"type", u8"the unqualified type name (e.g. \"JsonValue\")", true)
                .Str(u8"namespace", u8"the exact namespace, to disambiguate a duplicated name")
                .Build(),
                foundation::mcp::ToolAnnotations::ReadOnly(),
            [reg](const JsonValue& args) -> ToolResult
            {
                const String typeName = args.Get(u8"type").AsString();
                const String ns = args.Get(u8"namespace").AsString();
                const TypeInfo* type = nullptr;
                if (!ns.IsEmpty())
                {
                    type = reg->FindByName(reinterpret_cast<const char*>(ns.CStr()),
                                           reinterpret_cast<const char*>(typeName.CStr()));
                }
                else
                {
                    const Array<const TypeInfo*>& all = reg->All();
                    for (usize i = 0; i < all.Size(); ++i)
                    {
                        if (detail::NameEquals(all[i]->name, typeName.AsView()))
                        {
                            type = all[i];
                            break;
                        }
                    }
                }
                if (type == nullptr)
                {
                    return Err(Format(u8"unknown type '{}'", typeName.AsView()));
                }
                return detail::DescribeType(*type, *reg);
            });
    }
}
