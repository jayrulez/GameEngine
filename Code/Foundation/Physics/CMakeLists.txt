# Foundation::Physics - `foundation.physics`: the Jolt-backed rigid-body core.
# Jolt is private: JPH types never cross the module surface.
util_add_engine_library(Physics ALIAS Foundation::Physics)
target_link_libraries(Physics
    PUBLIC Foundation::Core Foundation::Policy
    PRIVATE ThirdParty::Jolt)

target_sources(Physics
    PRIVATE
        PhysicsWorldImpl.cpp   # module implementation unit: all Jolt contact (see PhysicsWorld.cppm)
    PUBLIC
        FILE_SET physics_modules TYPE CXX_MODULES FILES
            PhysicsModule.cppm
            PhysicsWorld.cppm)

