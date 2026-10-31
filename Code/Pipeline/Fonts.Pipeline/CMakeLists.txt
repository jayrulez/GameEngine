# Pipeline::Fonts - `fonts.pipeline` (FontAsset, its importer and FontAssetBuilder: the
# source and cook side of fonts). Tooling only; requires the msdfgen distance-field baker.

util_add_engine_library(Fonts.Pipeline ALIAS Pipeline::Fonts)
target_link_libraries(Fonts.Pipeline
    PUBLIC Foundation::Core Pipeline::Core Pipeline::Importer
           Foundation::Fonts Foundation::Fonts.TrueType Foundation::Fonts.Coverage
           Foundation::Fonts.Coverage.Baker Foundation::Fonts.DistanceField
           Foundation::Fonts.DistanceField.Baker Foundation::Fonts.Resource Foundation::Fonts.HarfBuzz
           Foundation::Content Foundation::Policy)

target_sources(Fonts.Pipeline
    PRIVATE
        FontAssetImpl.cpp
    PUBLIC
        FILE_SET fonts_editor_modules TYPE CXX_MODULES FILES
            FontAsset.cppm)
