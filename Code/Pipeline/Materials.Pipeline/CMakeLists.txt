# Pipeline::Materials - `materials.pipeline` (tooling: MaterialAsset ->
# MaterialSource cook). Never linked by the runtime.
util_add_engine_library(Materials.Pipeline ALIAS Pipeline::Materials)
target_link_libraries(Materials.Pipeline
    PUBLIC Foundation::Core Pipeline::Core Foundation::Content
           Foundation::Materials Foundation::Materials.Resource Foundation::Shaders
           Foundation::Shaders.Resource Foundation::Policy)

target_sources(Materials.Pipeline
    PRIVATE
        MaterialsCreatorsImpl.cpp # File > New, asset_create
    PRIVATE
        MaterialAssetImpl.cpp   # MaterialAsset reflection body (Nested source; kept out of the interface for GCC)
    PUBLIC
        FILE_SET material_editor_modules TYPE CXX_MODULES FILES
            MaterialAsset.cppm)

