// Draconic::FontsEditor - reflection implementation unit: FontAsset's reflected surface.
//
// Kept OUT of the FontAsset.cppm interface (DRACONIC_REFLECT bodies make GCC emit a gcm cluster;
// see gcc-module-interface-hygiene). The class declares identity via DRACONIC_OBJECT in the
// interface; this unit defines FontAsset::StaticType() WITH properties + tooling attributes, plus
// the FontBakeMode enum reflection. The `sizes` ramp (Array<f32>) is left unreflected for now -
// array container properties need editor support beyond P1's flat-field pass. Reflection track P1.

module;
#include "Draconic.Core/Prelude.h"
#include "Draconic.Core/Reflection/Reflect.h"

module draconic.fonts.editor;

import draconic.core;
import draconic.editor;

using namespace draconic::core;

namespace draconic::fonts
{
    DRACONIC_REFLECT_ENUM(FontBakeMode, "draconic::fonts")
    {
        builder.Value("RasterRamp", FontBakeMode::RasterRamp);
        builder.Value("DistanceField", FontBakeMode::DistanceField);
    }

    DRACONIC_REFLECT(FontAsset, "draconic::fonts")
    {
        builder.Attribute("displayName", String(u8"Font"))
            .Attribute("category", String(u8"Fonts"))
            .Property<&FontAsset::family>("family")
            .PropAttribute("displayName", String(u8"Family"))
            .PropAttribute("description",
                           String(u8"Runtime family name (empty = the file's own family)"))
            .Property<&FontAsset::mode>("mode")
            .PropAttribute("displayName", String(u8"Bake Mode"))
            .Property<&FontAsset::dfSize>("dfSize")
            .PropAttribute("displayName", String(u8"Distance-Field Size"))
            .PropAttribute("range", Float4{8.0f, 128.0f, 1.0f, 0.0f})
            .PropAttribute("visibleWhen", String(u8"mode=1")) // DistanceField only
            .Property<&FontAsset::firstCodepoint>("firstCodepoint")
            .PropAttribute("displayName", String(u8"First Codepoint"))
            .Property<&FontAsset::lastCodepoint>("lastCodepoint")
            .PropAttribute("displayName", String(u8"Last Codepoint"))
            .Property<&FontAsset::atlasWidth>("atlasWidth")
            .PropAttribute("displayName", String(u8"Atlas Width"))
            .Property<&FontAsset::atlasHeight>("atlasHeight")
            .PropAttribute("displayName", String(u8"Atlas Height"));
    }

    void RegisterFontAssetReflection()
    {
        static const bool once = []()
        {
            DraconicRegisterEnum_FontBakeMode();
            return true;
        }();
        (void)once;
    }
}
