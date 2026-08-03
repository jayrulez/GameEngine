// Draconic::Image - reflection implementation unit: enum reflection bodies.
//
// Kept OUT of the :image_data interface partition (DRACONIC_REFLECT_* bodies make GCC emit a gcm
// cluster; see gcc-module-interface-hygiene). ImageData.cppm declares RegisterImageReflection();
// this unit defines it + the DraconicRegisterEnum_ImageColorSpace body. Reflection track P1.

module;
#include "Draconic.Core/Prelude.h"
#include "Draconic.Core/Reflection/Reflect.h"

module draconic.image;

import draconic.core;

using namespace draconic::core;

namespace draconic::image
{
    DRACONIC_REFLECT_ENUM(ImageColorSpace, "draconic::image")
    {
        builder.Value("Srgb", ImageColorSpace::Srgb);
        builder.Value("Linear", ImageColorSpace::Linear);
    }

    void RegisterImageReflection()
    {
        static const bool once = []()
        {
            DraconicRegisterEnum_ImageColorSpace();
            return true;
        }();
        (void)once;
    }
}
