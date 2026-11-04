// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Pipeline::Script - reflection implementation unit: ScriptClassAsset's reflected surface.
//
// Kept out of the ScriptAsset.cppm interface (REFLECT_MEMBERS bodies make GCC emit a gcm cluster).
// ScriptClassAsset::StaticType() gains its `language` property here. There are no enums, so the
// reflection comes with StaticType() and needs no registrar.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

#include <cstring>

module script.pipeline;

import foundation.core;
import pipeline.core;

using namespace foundation::core;
using namespace foundation::script;

namespace pipeline{
    ScriptLanguageCookRegistry& ScriptLanguageCookRegistry::Get()
    {
        static ScriptLanguageCookRegistry instance;
        return instance;
    }

    u64 ScriptSurfaceHash(const TypeRegistry& registry)
    {
        const auto text = [](u64 hash, const char* value)
        {
            const usize size = value != nullptr ? std::strlen(value) : 0;
            return HashBytes(value, size + 1, hash); // the terminator keeps "ab"+"c" apart from "a"+"bc"
        };
        const auto number = [](u64 hash, u64 value) { return HashBytes(&value, sizeof(value), hash); };
        u64 sum = 0;
        for (const TypeInfo* type : registry.All())
        {
            if (!(registry.DomainOf(type->id) == kRuntimeTypeDomain))
            {
                continue;
            }
            u64 h = number(kFnv1a64OffsetBasis, type->id);
            h = number(h, type->base != nullptr ? type->base->id : 0);
            for (u32 i = 0; i < type->propertyCount; ++i)
            {
                h = text(h, type->properties[i].name);
                h = number(h, type->properties[i].type != nullptr ? type->properties[i].type->id : 0);
            }
            for (u32 i = 0; i < type->methodCount; ++i)
            {
                h = text(h, type->methods[i].name);
                h = number(h, (static_cast<u64>(type->methods[i].paramCount) << 2) |
                                  (type->methods[i].isStatic ? 2u : 0u) | (type->methods[i].isConst ? 1u : 0u));
            }
            for (u32 i = 0; i < type->enumeratorCount; ++i)
            {
                h = text(h, type->enumerators[i].name);
            }
            h = number(h, type->constructorCount);
            sum += HashInteger(h); // a sum: the registration order does not matter
        }
        return sum;
    }

    REFLECT_MEMBERS(ScriptClassAsset, "rtti::pipeline::script")
    {
        builder.Attribute("displayName", String(u8"Script"))
            .Attribute("category", String(u8"Scripting"))
            .Property<&ScriptClassAsset::language>("language")
            .PropAttribute("displayName", String(u8"Language"))
            .PropAttribute("description", String(u8"Backend id (e.g. \"angelscript\", \"luau\")"));
    }
}
