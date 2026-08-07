# Draconic::Physics.Resource - `draconic.physics.resource` (cooked collision shapes + physical materials).
add_library(Draconic.Physics.Resource STATIC)
add_library(Draconic::Physics.Resource ALIAS Draconic.Physics.Resource)
target_link_libraries(Draconic.Physics.Resource PUBLIC
    Draconic::Foundation Draconic::Resource Draconic::Content Draconic::Policy)
target_sources(Draconic.Physics.Resource PUBLIC FILE_SET physics_resource_modules TYPE CXX_MODULES FILES PhysicsResource.cppm)
