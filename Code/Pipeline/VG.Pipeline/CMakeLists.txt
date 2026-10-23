# Pipeline::VG - `vg.pipeline` (tooling: the vector image asset, its validating builder and the .svg
# importer).
util_add_engine_library(VG.Pipeline ALIAS Pipeline::VG)
target_link_libraries(VG.Pipeline PUBLIC
    Foundation::Core Pipeline::Core Pipeline::Importer Foundation::Content Foundation::VG.SVG Foundation::VG.Resource
    Foundation::Policy)
target_sources(VG.Pipeline
    PRIVATE
        VGAssetImpl.cpp
    PUBLIC FILE_SET vg_editor_modules TYPE CXX_MODULES FILES VGAsset.cppm)
