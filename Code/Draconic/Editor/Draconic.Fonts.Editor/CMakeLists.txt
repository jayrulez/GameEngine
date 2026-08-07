# Draconic::Fonts.Editor - `draconic.fonts.editor` (FontAsset + importer + FontAssetBuilder:
# the source/cook tier of the fonts triad). Tooling only; requires the msdfgen DF baker.

add_library(Draconic.Fonts.Editor STATIC)
add_library(Draconic::Fonts.Editor ALIAS Draconic.Fonts.Editor)

target_link_libraries(Draconic.Fonts.Editor
    PUBLIC Draconic::Foundation Draconic::Editor Draconic::Editor.Core
           Draconic::Fonts Draconic::Fonts.TTF Draconic::Fonts.IO Draconic::Fonts.Baked
           Draconic::Fonts.Importer Draconic::Fonts.DistanceField
           Draconic::Fonts.DistanceField.Baker Draconic::Fonts.Resource
           Draconic::Content Draconic::Policy)

target_sources(Draconic.Fonts.Editor
    PRIVATE
        FontAssetImpl.cpp
    PUBLIC
        FILE_SET fonts_editor_modules TYPE CXX_MODULES FILES
            FontAsset.cppm)
