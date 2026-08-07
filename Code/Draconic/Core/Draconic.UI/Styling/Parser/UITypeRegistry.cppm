// Draconic UI - :ui_type_registry partition
//
// Maps short string names to View types (our RTTI TypeInfo). Used by the .sss parser for element
// selectors and by the .sml loader for element resolution. User controls register via
// UITypeRegistry::Register("MyControl", &MyControl::StaticType()). Ported from
// Sedulous.UI/src/Styling/Parser/UITypeRegistry.bf.
//
// Divergences (language): Beef `Type` -> const foundation::TypeInfo*; the static Dictionary lives in a
// function-local static HashMap (avoids C++ static-init-order issues). RegisterBuiltins() is DEFERRED
// until the control classes land (it references View subclasses not yet ported); the parser and its
// tests register the types they need explicitly for now.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.ui:ui_type_registry;

import draconic.foundation; // TypeInfo, HashMap, String, StringView

using namespace draconic::foundation;

namespace draconic::ui::detail
{
    inline HashMap<String, const TypeInfo*>& UITypeMap()
    {
        static HashMap<String, const TypeInfo*> types;
        return types;
    }
}

export namespace draconic::ui
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
    };
}
