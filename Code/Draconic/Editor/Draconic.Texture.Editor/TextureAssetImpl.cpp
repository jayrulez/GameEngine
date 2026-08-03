// Draconic::TextureEditor - reflection implementation unit: TextureAsset's reflected surface.
//
// Kept OUT of the TextureAsset.cppm interface (DRACONIC_REFLECT bodies make GCC emit a gcm
// cluster; see gcc-module-interface-hygiene). The class declares its identity via DRACONIC_OBJECT
// in the interface; this unit defines TextureAsset::StaticType() WITH properties + tooling
// attributes, so the generic asset page and the script backends see the authored surface.
// The enum property types (TextureShape/Filter/Wrap, ImageColorSpace) are reflected in their
// owning modules and registered by RegisterTextureAsset. Reflection track P1.

module;
#include "Draconic.Core/Prelude.h"
#include "Draconic.Core/Reflection/Reflect.h"

module draconic.texture.editor;

import draconic.core;
import draconic.editor;
import draconic.texture;
import draconic.image;

using namespace draconic::core;

namespace draconic::texture
{
    DRACONIC_REFLECT(TextureAsset, "draconic::texture")
    {
        builder.Attribute("displayName", String(u8"Texture"))
            .Attribute("category", String(u8"Textures"))
            .Property<&TextureAsset::colorSpace>("colorSpace")
            .PropAttribute("displayName", String(u8"Color Space"))
            .Property<&TextureAsset::shape>("shape")
            .PropAttribute("displayName", String(u8"Shape"))
            .Property<&TextureAsset::minFilter>("minFilter")
            .PropAttribute("displayName", String(u8"Min Filter"))
            .Property<&TextureAsset::magFilter>("magFilter")
            .PropAttribute("displayName", String(u8"Mag Filter"))
            .Property<&TextureAsset::wrapU>("wrapU")
            .PropAttribute("displayName", String(u8"Wrap U"))
            .Property<&TextureAsset::wrapV>("wrapV")
            .PropAttribute("displayName", String(u8"Wrap V"))
            .Property<&TextureAsset::wrapW>("wrapW")
            .PropAttribute("displayName", String(u8"Wrap W"))
            .Property<&TextureAsset::generateMipmaps>("generateMipmaps")
            .PropAttribute("displayName", String(u8"Generate Mipmaps"))
            .Property<&TextureAsset::anisotropy>("anisotropy")
            .PropAttribute("displayName", String(u8"Anisotropy"))
            .PropAttribute("range", Float4{1.0f, 16.0f, 1.0f, 0.0f})
            .Property<&TextureAsset::embeddedWidth>("embeddedWidth")
            .PropAttribute("displayName", String(u8"Embedded Width"))
            .PropAttribute("description",
                           String(u8"Model-import embedded pixels (0 for file-backed textures)"))
            .Property<&TextureAsset::embeddedHeight>("embeddedHeight")
            .PropAttribute("displayName", String(u8"Embedded Height"))
            .PropAttribute("description",
                           String(u8"Model-import embedded pixels (0 for file-backed textures)"));
    }
}
