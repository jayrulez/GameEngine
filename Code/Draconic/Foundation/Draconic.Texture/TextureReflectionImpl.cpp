// Draconic::Texture - reflection implementation unit: enum reflection bodies.
//
// Kept OUT of the :types interface partition (DRACONIC_REFLECT_* bodies make GCC emit a gcm
// cluster; see gcc-module-interface-hygiene). Types.cppm declares RegisterTextureReflection();
// this unit defines it + the DraconicRegisterEnum_* bodies. Reflection track P1.

module;
#include "Draconic.Core/Prelude.h"
#include "Draconic.Core/Reflection/Reflect.h"

module draconic.texture;

import draconic.core;

using namespace draconic::core;

namespace draconic::texture
{
    DRACONIC_REFLECT_ENUM(TextureShape, "draconic::texture")
    {
        builder.Value("Texture2D", TextureShape::Texture2D);
        builder.Value("Texture2DArray", TextureShape::Texture2DArray);
        builder.Value("Texture3D", TextureShape::Texture3D);
        builder.Value("Cubemap", TextureShape::Cubemap);
        builder.Value("CubemapArray", TextureShape::CubemapArray);
    }

    DRACONIC_REFLECT_ENUM(TextureFilter, "draconic::texture")
    {
        builder.Value("Nearest", TextureFilter::Nearest);
        builder.Value("Linear", TextureFilter::Linear);
        builder.Value("MipmapNearest", TextureFilter::MipmapNearest);
        builder.Value("MipmapLinear", TextureFilter::MipmapLinear);
    }

    DRACONIC_REFLECT_ENUM(TextureWrap, "draconic::texture")
    {
        builder.Value("Repeat", TextureWrap::Repeat);
        builder.Value("ClampToEdge", TextureWrap::ClampToEdge);
        builder.Value("ClampToBorder", TextureWrap::ClampToBorder);
        builder.Value("MirroredRepeat", TextureWrap::MirroredRepeat);
    }

    void RegisterTextureReflection()
    {
        static const bool once = []()
        {
            DraconicRegisterEnum_TextureShape();
            DraconicRegisterEnum_TextureFilter();
            DraconicRegisterEnum_TextureWrap();
            return true;
        }();
        (void)once;
    }
}
