// Draconic::UIEditor - reflection implementation unit: UI asset reflected surfaces.
//
// Kept OUT of the UIAsset.cppm interface (DRACONIC_REFLECT bodies make GCC emit a gcm cluster;
// see gcc-module-interface-hygiene). UIDocumentAsset (markup) and UIThemeAsset (stylesheet) gain
// their string properties here. No enums, so the reflection rides StaticType() with no registrar
// change. Reflection track P1.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

module draconic.ui.editor;

import draconic.foundation;
import draconic.editor;

using namespace draconic::foundation;

namespace draconic::ui
{
    DRACONIC_REFLECT(UIDocumentAsset, "draconic::ui")
    {
        builder.Attribute("displayName", String(u8"UI Document"))
            .Attribute("category", String(u8"UI"))
            .Property<&UIDocumentAsset::markup>("markup")
            .PropAttribute("displayName", String(u8"Markup"));
    }

    DRACONIC_REFLECT(UIThemeAsset, "draconic::ui")
    {
        builder.Attribute("displayName", String(u8"UI Theme"))
            .Attribute("category", String(u8"UI"))
            .Property<&UIThemeAsset::stylesheet>("stylesheet")
            .PropAttribute("displayName", String(u8"Stylesheet"));
    }
}
