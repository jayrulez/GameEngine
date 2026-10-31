# Pipeline::Input - `input.pipeline`: authored input-map asset and its builder (tooling).
util_add_engine_library(Input.Pipeline ALIAS Pipeline::Input)
target_link_libraries(Input.Pipeline
    PUBLIC Foundation::Core Foundation::Content Pipeline::Core Foundation::Input
           Foundation::Input.Resource Foundation::Policy)

target_sources(Input.Pipeline
    PRIVATE
        InputCreatorsImpl.cpp # File > New, asset_create
    PRIVATE
        InputMapAssetImpl.cpp   # InputMapAsset reflection body (Nested map; kept out of the interface for GCC)
    PUBLIC
        FILE_SET input_editor_modules TYPE CXX_MODULES FILES
            InputMapAsset.cppm)
