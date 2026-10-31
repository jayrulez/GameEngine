# Pipeline::Heightfield - `heightfield.pipeline` (tooling: HeightfieldAsset, its builder and importer).
util_add_engine_library(Heightfield.Pipeline ALIAS Pipeline::Heightfield)
target_link_libraries(Heightfield.Pipeline PUBLIC
    Foundation::Core Foundation::VFS Pipeline::Core Pipeline::Importer
    Foundation::Image Foundation::Image.IO
    Foundation::Heightfield Foundation::Heightfield.Resource
    Foundation::Content Foundation::Policy)
target_sources(Heightfield.Pipeline
    PRIVATE
        HeightfieldCreatorsImpl.cpp # File > New, asset_create
    PRIVATE
        HeightfieldAssetImpl.cpp
    PUBLIC FILE_SET heightfield_pipeline_modules TYPE CXX_MODULES FILES HeightfieldAsset.cppm)
