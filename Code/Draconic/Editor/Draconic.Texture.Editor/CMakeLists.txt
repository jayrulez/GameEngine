# Draconic::Texture.Editor - `draconic.texture.editor` (tooling: TextureAsset + builder + importer).
add_library(Draconic.Texture.Editor STATIC)
add_library(Draconic::Texture.Editor ALIAS Draconic.Texture.Editor)
target_link_libraries(Draconic.Texture.Editor PUBLIC
    Draconic::Foundation Draconic::Editor Draconic::Editor.Core Draconic::RHI Draconic::Texture Draconic::Texture.Resource
    Draconic::Image Draconic::Image.IO Draconic::Content Draconic::Policy)
target_sources(Draconic.Texture.Editor
    PRIVATE
        TextureAssetImpl.cpp
    PUBLIC FILE_SET texture_editor_modules TYPE CXX_MODULES FILES TextureAsset.cppm)

