# Pipeline::Geometry - `geometry.pipeline` (tooling: Static/SkinnedMeshAsset ->
# Static/SkinnedMeshSource cook). Never linked by the runtime.
util_add_engine_library(Geometry.Pipeline ALIAS Pipeline::Geometry)
target_link_libraries(Geometry.Pipeline
    PUBLIC Foundation::Core Pipeline::Core Foundation::Content
           Foundation::Geometry Foundation::Geometry.Resource Foundation::Policy
    # Cook-time only; a PRIVATE link and an include in the implementation unit keep the vendored
    # header out of the module interface and out of every consumer's link line.
    PRIVATE ThirdParty::MeshOptimizer)

target_sources(Geometry.Pipeline
    PRIVATE
        GeometryCreatorsImpl.cpp # File > New, asset_create
    PRIVATE
        MeshOptimizeImpl.cpp   # optimization pass (meshoptimizer lives here only)
    PUBLIC
        FILE_SET mesh_editor_modules TYPE CXX_MODULES FILES
            MeshAsset.cppm)

