# Pipeline::Physics - `physics.pipeline` (tooling: collision shape and physical material assets and builders).
util_add_engine_library(Physics.Pipeline ALIAS Pipeline::Physics)
target_link_libraries(Physics.Pipeline PUBLIC
    Foundation::Core Pipeline::Core Foundation::Content Foundation::Geometry Pipeline::Geometry
    Foundation::Geometry.Resource Foundation::Physics Foundation::Physics.Resource Foundation::Policy)
target_sources(Physics.Pipeline
    PRIVATE
        PhysicsCreatorsImpl.cpp # File > New, asset_create
    PRIVATE
        PhysicsAssetImpl.cpp
    PUBLIC FILE_SET physics_editor_modules TYPE CXX_MODULES FILES PhysicsAsset.cppm)

