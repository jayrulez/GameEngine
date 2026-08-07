// Draconic::ShadersEditor - reflection implementation unit: ShaderAsset's reflected surface.
//
// Kept OUT of the ShaderAsset.cppm interface (DRACONIC_REFLECT bodies make GCC emit a gcm cluster;
// see gcc-module-interface-hygiene). ShaderAsset::StaticType() gains its authored string
// properties here. No enums, so the type reflection rides StaticType() with no registrar change.
// Reflection track P1.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

module draconic.shaders.editor;

import draconic.foundation;
import draconic.editor;

using namespace draconic::foundation;

namespace draconic::shaders
{
    DRACONIC_REFLECT(ShaderAsset, "draconic::shaders")
    {
        builder.Attribute("displayName", String(u8"Shader"))
            .Attribute("category", String(u8"Rendering"))
            .Property<&ShaderAsset::name>("name")
            .PropAttribute("displayName", String(u8"Name"))
            .PropAttribute("description", String(u8"Logical name materials reference"))
            .Property<&ShaderAsset::fragmentFile>("fragmentFile")
            .PropAttribute("displayName", String(u8"Fragment File"))
            .PropAttribute("description", String(u8"Fragment-stage HLSL (vertex = the asset file)"));
    }
}
