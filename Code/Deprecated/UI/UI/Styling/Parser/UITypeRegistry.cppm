// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - :ui_type_registry partition
//
// Maps short string names to View types (our RTTI TypeInfo). Used by the .sss parser for element
// selectors and by the .sml loader for element resolution. User controls register via
// UITypeRegistry::Register("MyControl", &MyControl::StaticType()). Ported from
// Sedulous.UI/src/Styling/Parser/UITypeRegistry.bf.
//
// Divergences (language): Beef `Type` -> const core::TypeInfo*; the static Dictionary lives in a
// function-local static HashMap (avoids C++ static-init-order issues). RegisterBuiltins() is not
// implemented while the control classes are unported (it references View subclasses not yet
// present); the parser and its tests register the types they need explicitly.

module;
#include "Core/Prelude.h"

export module foundation.ui:ui_type_registry;

import foundation.core; // TypeInfo, HashMap, String, StringView

using namespace foundation::core;

namespace foundation::ui::detail
{
    // NON-inline (UiRegistryStateImpl.cpp): shared-libraries.md rendezvous rule.
    [[nodiscard]] HashMap<String, const TypeInfo*>& UITypeMap();
}

export namespace foundation::ui
{
    struct UITypeRegistry
    {
        /// Register a type with a name. Overwrites if the name already exists.
        static void Register(StringView name, const TypeInfo* type)
        {
            detail::UITypeMap().InsertOrAssign(String(name), type);
        }

        /// Look up a type by name. Returns null if not found.
        [[nodiscard]] static const TypeInfo* Resolve(StringView name)
        {
            if (const TypeInfo* const* t = detail::UITypeMap().Find(String(name)))
            {
                return *t;
            }
            return nullptr;
        }

        /// Number of registered types.
        [[nodiscard]] static usize Count() { return detail::UITypeMap().Size(); }

        /// Register all built-in View/layout/control type names (idempotent). Body in an impl unit
        /// (UITypeRegistryImpl.cpp) that reaches every control via the module - so element selectors like
        /// `ComboBox::arrow` in a .sss resolve to a concrete type instead of matching everything.
        static void RegisterBuiltins();

    private:
        static void RegisterBuiltinsBody(); // impl unit
    };
}
