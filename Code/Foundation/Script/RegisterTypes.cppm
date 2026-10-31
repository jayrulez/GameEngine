// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Script - :script_register partition
//
// Bridges reflection to scripting: registers every type in a TypeRegistry with a
// script manager. Call after the engine's RegisterCoreTypes() (and any
// higher-layer registration) to expose them to scripts in one shot.

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"
#include "Core/Debug/Assert.h"

export module foundation.script:script_register;

import foundation.core;
import :script_manager;

namespace core = foundation::core;

export namespace foundation::script
{
    // The name a method is spelled by on the script surface: its reflected overloadedName when set,
    // else its C++ name. This is how the scripting layer interprets the opaque overloadedName
    // metadata Core stores. Every backend binds by this name, so an overload is spelled the same
    // in every language, and the backend-neutral surface script_api reports matches all of them.
    [[nodiscard]] inline const char* ScriptMethodName(const core::MethodInfo& method) noexcept
    {
        return (method.overloadedName != nullptr && method.overloadedName[0] != '\0')
                   ? method.overloadedName
                   : method.name;
    }

    // The overload rule keys on a script method's identity (name, arity, staticness): no two
    // methods on a type may share all three. The same name at different arities is a legal arity
    // family, which every backend dispatches by argument count (AngelScript overloads natively,
    // the Luau thunk switches on argc) and typed declarations express as an overloaded function
    // type. Only a same-name, same-arity clash is ambiguous on a dynamically typed surface (a
    // number picks neither f32 nor i32), and it has to be split by distinct OverloadedName()s.
    // Returns the first colliding script name, or nullptr when the type is clean.
    [[nodiscard]] inline const char*
    FindScriptMethodNameCollision(const core::TypeInfo& type) noexcept
    {
        const auto equal = [](const char* a, const char* b) noexcept
        {
            if (a == b)
            {
                return true;
            }
            if (a == nullptr || b == nullptr)
            {
                return false;
            }
            while (*a != '\0' && *a == *b)
            {
                ++a;
                ++b;
            }
            return *a == *b;
        };
        const core::Span<const core::MethodInfo> methods = core::Methods(type);
        for (core::usize i = 0; i < methods.Size(); ++i)
        {
            const char* nameI = ScriptMethodName(methods[i]);
            for (core::usize j = i + 1; j < methods.Size(); ++j)
            {
                // A collision is the same (name, arity, static). Different arity is a legal family.
                if (methods[i].isStatic == methods[j].isStatic &&
                    methods[i].paramCount == methods[j].paramCount &&
                    equal(nameI, ScriptMethodName(methods[j])))
                {
                    return nameI;
                }
            }
        }
        return nullptr;
    }

    // FinalizeTypes calls this over the types a backend has registered. It fails loudly (logs the
    // offending type and name, then traps) on the first overload that lacks distinct
    // overloadedNames, which turns a silent one-overload-wins collision into an immediate
    // registration-time error on every backend.
    inline void ValidateScriptMethodNames(core::Span<const core::TypeInfo* const> types)
    {
        for (const core::TypeInfo* type : types)
        {
            if (type == nullptr)
            {
                continue;
            }
            const char* collision = FindScriptMethodNameCollision(*type);
            if (collision != nullptr)
            {
                LOG_ERROR(u8"Script",
                          u8"reflected type '{}' binds two methods to the script name '{}' - give "
                          u8"each overload a distinct .OverloadedName(...)",
                          core::StringView(reinterpret_cast<const core::utf8char*>(type->name)),
                          core::StringView(reinterpret_cast<const core::utf8char*>(collision)));
                DIAGNOSTIC_ASSERT(collision == nullptr &&
                                  "overloaded methods need distinct overloadedName for scripting");
            }
        }
    }

    // The name a type is spelled by on the script surface: its "scriptName" class-attribute alias when
    // set, else its C++ name. Core knows nothing of "scriptName" (it stores a generic key and value,
    // like the editor's displayName); the scripting layer interprets it, and every backend binds the
    // class under this name. It lets a C++ type expose a script-idiomatic name (`run` for class Run)
    // without touching its native identity: serialization and TypeRegistry FindByName stay on the C++
    // `name`, which never aliases.
    inline constexpr const char* kScriptNameAttribute = "scriptName";

    [[nodiscard]] inline const char* ScriptTypeName(const core::TypeInfo& type) noexcept
    {
        const core::Variant* alias = core::FindAttribute(type, kScriptNameAttribute);
        if (alias != nullptr && alias->Is<const char*>())
        {
            const char* aliasName = alias->Get<const char*>();
            if (aliasName != nullptr && aliasName[0] != '\0')
            {
                return aliasName;
            }
        }
        return type.name;
    }

    // Two types may not share a script-facing type name (a C++ class name or a scriptName alias):
    // they cannot both bind as `X`, and a silent last-wins would shadow one, as when a facade claims a
    // reserved name like `run` that something else also claims. Returns the first colliding script
    // name, or nullptr when the set is clean. The type-level counterpart of
    // FindScriptMethodNameCollision.
    [[nodiscard]] inline const char*
    FindScriptTypeNameCollision(core::Span<const core::TypeInfo* const> types) noexcept
    {
        const auto equal = [](const char* a, const char* b) noexcept
        {
            if (a == b)
            {
                return true;
            }
            if (a == nullptr || b == nullptr)
            {
                return false;
            }
            while (*a != '\0' && *a == *b)
            {
                ++a;
                ++b;
            }
            return *a == *b;
        };
        for (core::usize i = 0; i < types.Size(); ++i)
        {
            if (types[i] == nullptr)
            {
                continue;
            }
            const char* nameI = ScriptTypeName(*types[i]);
            for (core::usize j = i + 1; j < types.Size(); ++j)
            {
                if (types[j] != nullptr && equal(nameI, ScriptTypeName(*types[j])))
                {
                    return nameI;
                }
            }
        }
        return nullptr;
    }

    // FinalizeTypes calls this over the types a backend has registered: it fails loudly on the first
    // duplicate script-facing type name, as ValidateScriptMethodNames does for methods.
    inline void ValidateScriptTypeNames(core::Span<const core::TypeInfo* const> types)
    {
        const char* collision = FindScriptTypeNameCollision(types);
        if (collision != nullptr)
        {
            LOG_ERROR(u8"Script",
                      u8"two reflected types bind to the same script name '{}' (a class name or a "
                      u8"scriptName alias) - names must be unique across the bound surface",
                      core::StringView(reinterpret_cast<const core::utf8char*>(collision)));
            DIAGNOSTIC_ASSERT(collision == nullptr &&
                              "duplicate script-facing type name (class name or scriptName)");
        }
    }

    inline void
    RegisterReflectedTypes(IScriptManager& manager,
                           const core::TypeRegistry& registry = core::GlobalTypeRegistry())
    {
        for (const core::TypeInfo* type : registry.All())
        {
            manager.RegisterType(*type);
        }
        manager.FinalizeTypes(); // two-phase backends emit here (declare all, then bind)
    }
}
