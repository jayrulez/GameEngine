// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - :ui_type_registry partition
//
// Maps short string names to View types (our RTTI TypeInfo). Used by the .sss parser for element
// selectors and by the .sml loader for element resolution. User controls register via
// UITypeRegistry::Register("MyControl", &MyControl::StaticType()). The map is a function-local
// static, which avoids static-init-order issues.

module;
#include "Core/Prelude.h"

export module foundation.ui:ui_type_registry;

import foundation.core;

using namespace foundation::core;

namespace foundation::ui::detail
{
    // Defined out of line in UiRegistryStateImpl.cpp: an inline function-local static would give
    // each shared library its own map.
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

        /// Register all built-in view, layout and control type names (idempotent). The body is in an
        /// implementation unit (UITypeRegistryImpl.cpp) that reaches every control through the module,
        /// so element selectors like `ComboBox::arrow` in a .sss resolve to a concrete type instead of
        /// matching everything.
        static void RegisterBuiltins();

    private:
        static void RegisterBuiltinsBody(); // in the implementation unit
    };
}
