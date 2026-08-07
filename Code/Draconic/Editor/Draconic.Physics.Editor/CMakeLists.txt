# Draconic::Physics.Editor - `draconic.physics.editor` (tooling: collision shape + physical material assets/builders).
add_library(Draconic.Physics.Editor STATIC)
add_library(Draconic::Physics.Editor ALIAS Draconic.Physics.Editor)
target_link_libraries(Draconic.Physics.Editor PUBLIC
    Draconic::Foundation Draconic::Editor Draconic::Content Draconic::Geometry Draconic::Geometry.Editor
    Draconic::Geometry.Resource Draconic::Physics Draconic::Physics.Resource Draconic::Policy)
target_sources(Draconic.Physics.Editor
    PRIVATE
        PhysicsAssetImpl.cpp
    PUBLIC FILE_SET physics_editor_modules TYPE CXX_MODULES FILES PhysicsAsset.cppm)

