# Foundation::Physics.Resource - `foundation.physics.resource` (cooked collision shapes and physical materials).
util_add_engine_library(Physics.Resource ALIAS Foundation::Physics.Resource)
target_link_libraries(Physics.Resource PUBLIC
    Foundation::Core Foundation::Resource Foundation::Content Foundation::Policy)
target_sources(Physics.Resource PUBLIC FILE_SET physics_resource_modules TYPE CXX_MODULES FILES PhysicsResource.cppm)
