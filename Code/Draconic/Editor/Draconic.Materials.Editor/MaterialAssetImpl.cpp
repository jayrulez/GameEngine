// Draconic::MaterialEditor - reflection implementation unit: MaterialAsset's reflected surface.
//
// Kept OUT of the MaterialAsset.cppm interface (DRACONIC_REFLECT bodies make GCC emit a gcm
// cluster; see gcc-module-interface-hygiene). MaterialAsset wraps a MaterialSource BY VALUE, and
// MaterialSource derives Object (RefCounted deletes its copy ctor) so it cannot marshal through a
// Variant - it is exposed as a NESTED property (TypeBuilder::Nested): tooling reaches the member
// in place via address and recurses into MaterialSource's own reflected properties. This is the
// reflection-track "living proof" of the nested-member mechanism. Reflection track P1.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

module draconic.materials.editor;

import draconic.foundation;
import draconic.editor;
import draconic.materials.resource; // MaterialSource (the nested reflected type)

using namespace draconic::foundation;

namespace draconic::materials
{
    DRACONIC_REFLECT(MaterialAsset, "draconic::materials")
    {
        builder.Attribute("displayName", String(u8"Material"))
            .Attribute("category", String(u8"Materials"))
            .Nested<&MaterialAsset::source>("source")
            .PropAttribute("displayName", String(u8"Source"));
    }
}
