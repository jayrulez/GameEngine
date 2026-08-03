# Draconic::Texture - `draconic.texture` (texture descriptors + image->RHI format map).
# Ported from Sedulous.Textures (deps: RHI, Images).

add_library(Draconic.Texture STATIC)
add_library(Draconic::Texture ALIAS Draconic.Texture)

target_link_libraries(Draconic.Texture PUBLIC Draconic::Core Draconic::RHI Draconic::Image Draconic::Policy)

target_sources(Draconic.Texture
    PRIVATE
        TextureReflectionImpl.cpp
    PUBLIC
        FILE_SET texture_modules TYPE CXX_MODULES FILES
            Types.cppm
            FormatUtils.cppm
            TextureData.cppm
            TextureModule.cppm)

