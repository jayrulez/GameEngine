// Draconic Script - :script_register partition
//
// Bridges reflection -> scripting: registers every type in a TypeRegistry with a
// script manager. Call after the engine's RegisterFoundationTypes() (and any
// higher-layer registration) to expose them to scripts in one shot.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.script:script_register;

import draconic.foundation;
import :script_manager;

namespace foundation = draconic::foundation;

export namespace draconic::script
{
    inline void
    RegisterReflectedTypes(IScriptManager& manager,
                           const foundation::TypeRegistry& registry = foundation::GlobalTypeRegistry())
    {
        for (const foundation::TypeInfo* type : registry.All())
        {
            manager.RegisterType(*type);
        }
        manager.FinalizeTypes(); // two-phase backends emit here (declare-all, then bind)
    }
}
