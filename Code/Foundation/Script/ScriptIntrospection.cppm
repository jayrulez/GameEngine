// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Script - :script_introspection partition
//
// What is callable in a backend's language and how it is spelled, the accurate source
// for autocomplete. The reflection registry says what types exist; DescribeBoundApi (on
// IScriptManager) says what a backend actually bound and under what script-visible names.
// They differ per language (AngelScript spells a static as `Float3::Dot`), and a backend
// may silently fail to bind a type at all, which a conformance diff against the
// reflection registry catches.

module;
#include "Core/Prelude.h"

export module foundation.script:script_introspection;

import foundation.core;

namespace core = foundation::core;

export namespace foundation::script
{
    /// What kind of member a bound API entry is, in the language's own terms.
    enum class ScriptApiMemberKind : core::u8
    {
        Method,   // callable member or free function
        Property, // field-like accessor (get, or get and set)
        Constant, // named constant value
        Operator, // an operator the type takes (a + b), bound from a reflected one (MethodOperator)
    };

    /// How a script writes `op` ("+", "-", "*", "/", "-x", "=="); empty for None.
    [[nodiscard]] inline core::StringView OperatorSymbol(core::MethodOperator op) noexcept
    {
        switch (op)
        {
        case core::MethodOperator::Add:
            return u8"+";
        case core::MethodOperator::Subtract:
            return u8"-";
        case core::MethodOperator::Multiply:
            return u8"*";
        case core::MethodOperator::Divide:
            return u8"/";
        case core::MethodOperator::Negate:
            return u8"-x";
        case core::MethodOperator::Equals:
            return u8"==";
        case core::MethodOperator::None:
            break;
        }
        return {};
    }

    /// One script-visible member of a bound type, spelled the way the backend presents
    /// it (the `signature` is language-formatted, e.g. AngelScript
    /// `Float3@ Dot(const Float3&in, const Float3&in)`).
    struct ScriptApiMember
    {
        core::String name;
        core::String signature;
        bool isStatic = false;
        /// A property a script reads and cannot assign (PropertyFlags::ReadOnly): the backend
        /// bound no setter, so an agent reading the surface knows before it writes.
        bool readOnly = false;
        ScriptApiMemberKind kind = ScriptApiMemberKind::Method;
    };

    /// One script-visible type (or namespace) the backend bound, with its members. The
    /// `scriptName` is the exact name a script uses; `isNamespace` distinguishes a real
    /// class from a namespace a backend synthesizes for statics.
    struct ScriptApiType
    {
        core::String scriptName;
        // The reflected identity behind the binding (0 = none). Tooling maps it back to
        // registry metadata; for example, TypeRegistry::DomainOf marks editor-only bindings.
        core::TypeId typeId = 0;
        bool isNamespace = false;
        core::Array<ScriptApiMember> members;
    };
}
