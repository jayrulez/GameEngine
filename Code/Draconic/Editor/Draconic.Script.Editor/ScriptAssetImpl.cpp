// Draconic::ScriptEditor - reflection implementation unit: ScriptClassAsset's reflected surface.
//
// Kept OUT of the ScriptAsset.cppm interface (DRACONIC_REFLECT bodies make GCC emit a gcm cluster;
// see gcc-module-interface-hygiene). ScriptClassAsset::StaticType() gains its `language` property
// here. No enums, so the reflection rides StaticType() with no registrar change. Reflection
// track P1.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

module draconic.script.editor;

import draconic.foundation;
import draconic.editor;

using namespace draconic::foundation;

namespace draconic::script
{
    DRACONIC_REFLECT(ScriptClassAsset, "draconic::script")
    {
        builder.Attribute("displayName", String(u8"Script"))
            .Attribute("category", String(u8"Scripting"))
            .Property<&ScriptClassAsset::language>("language")
            .PropAttribute("displayName", String(u8"Language"))
            .PropAttribute("description", String(u8"Backend id (e.g. \"wren\", \"angelscript\")"));
    }
}
