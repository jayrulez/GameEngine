# Draconic::Image.Editor - `draconic.image.editor` (tooling: ImageAsset + builder).
add_library(Draconic.Image.Editor STATIC)
add_library(Draconic::Image.Editor ALIAS Draconic.Image.Editor)
target_link_libraries(Draconic.Image.Editor PUBLIC Draconic::Core Draconic::Editor Draconic::Image Draconic::Image.IO Draconic::Image.Resource Draconic::Content Draconic::Policy)
target_sources(Draconic.Image.Editor
    PRIVATE
        ImageAssetImpl.cpp
    PUBLIC FILE_SET image_editor_modules TYPE CXX_MODULES FILES ImageAsset.cppm)
