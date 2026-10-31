# Pipeline::Core - `pipeline.core` (asset-pipeline authoring base: Asset,
# builders and the builder registry). Tooling only; the runtime never links this.

util_add_engine_library(Pipeline.Core ALIAS Pipeline::Core)
target_link_libraries(Pipeline.Core PUBLIC Foundation::Core Foundation::Content Foundation::VFS Foundation::Policy)

target_sources(Pipeline.Core
    PRIVATE
        AssetImpl.cpp
    PUBLIC
        FILE_SET pipeline_modules TYPE CXX_MODULES FILES
            Asset.cppm
            AssetCreator.cppm)
