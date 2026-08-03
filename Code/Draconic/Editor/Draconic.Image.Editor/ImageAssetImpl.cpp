// Draconic::ImageEditor - reflection implementation unit: ImageAsset's reflected surface.
//
// Kept OUT of the ImageAsset.cppm interface (DRACONIC_REFLECT bodies make GCC emit a gcm cluster;
// see gcc-module-interface-hygiene). ImageAsset::StaticType() gains its colorSpace property here;
// the ImageColorSpace enum reflection lives in draconic.image (RegisterImageReflection).
// Reflection track P1.

module;
#include "Draconic.Core/Prelude.h"
#include "Draconic.Core/Reflection/Reflect.h"

module draconic.image.editor;

import draconic.core;
import draconic.editor;
import draconic.image;

using namespace draconic::core;

namespace draconic::image
{
    DRACONIC_REFLECT(ImageAsset, "draconic::image")
    {
        builder.Attribute("displayName", String(u8"Image"))
            .Attribute("category", String(u8"Textures"))
            .Property<&ImageAsset::colorSpace>("colorSpace")
            .PropAttribute("displayName", String(u8"Color Space"));
    }
}
