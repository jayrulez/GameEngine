// Draconic::InputEditor - reflection implementation unit: InputMapAsset's reflected surface.
//
// InputMapAsset wraps an InputMap by value; InputMap is a nested list-of-lists (sets -> actions ->
// bindings). It is exposed as a NESTED property (TypeBuilder::Nested) so tooling/scripting can
// traverse the whole tree in place via container reflection, without marshalling it by value. The
// input EDITOR page stays bespoke - this reflection is for scriptability, not a generated inspector.
// DRACONIC_REFLECT out of the interface (GCC module hygiene). Reflection track P2.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

module draconic.input.editor;

import draconic.foundation;
import draconic.editor;
import draconic.input; // InputMap (the nested reflected type)

using namespace draconic::foundation;

namespace draconic::input
{
    DRACONIC_REFLECT(InputMapAsset, "draconic::input")
    {
        builder.Attribute("displayName", String(u8"Input Map"))
            .Attribute("category", String(u8"Input"))
            .Nested<&InputMapAsset::m_map>("map")
            .PropAttribute("displayName", String(u8"Map"));
    }
}
