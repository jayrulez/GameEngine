// Draconic::Texture - :types partition
//
// Logical texture descriptors: shape, filter, wrap. Used by TextureResource to
// say how pixel data should be interpreted and sampled. Ported from
// Sedulous.Textures/TextureTypes.bf.

module;
#include "Draconic.Core/Prelude.h"

export module draconic.texture:types;

import draconic.core;

using namespace draconic::core;

export namespace draconic::texture
{
    // The logical shape of a texture asset.
    enum class TextureShape : u8
    {
        Texture2D,      // standard 2D image
        Texture2DArray, // multiple same-size layers
        Texture3D,      // volume
        Cubemap,        // 6 square faces (+X,-X,+Y,-Y,+Z,-Z)
        CubemapArray,   // array of cubemaps
    };

    enum class TextureFilter : u8
    {
        Nearest,
        Linear,
        MipmapNearest,
        MipmapLinear,
    };

    enum class TextureWrap : u8
    {
        Repeat,
        ClampToEdge,
        ClampToBorder,
        MirroredRepeat,
    };

    // Reflects TextureShape/TextureFilter/TextureWrap for tooling (enum-by-name dropdowns in the
    // generic asset page) + scripting. Idempotent; called by consumers that need the enum names
    // (the editor/cook/export tools via RegisterTextureAsset). Body lives in the impl unit
    // (gcc module-interface hygiene). Reflection track P1.
    void RegisterTextureReflection();
}
