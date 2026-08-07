# Draconic::Engine.Integration - `draconic.engine.integration`: cross-subsystem composition
# helpers owned by the app. Depends on BOTH sides of each bridge so neither subsystem gains a
# dependency on the other. First tenant: the physics-contact -> script bridge.
add_library(Draconic.Engine.Integration STATIC)
add_library(Draconic::Engine.Integration ALIAS Draconic.Engine.Integration)

target_link_libraries(Draconic.Engine.Integration
    PUBLIC Draconic::Foundation Draconic::Scene Draconic::Physics
           Draconic::Engine.Physics Draconic::Engine.Script Draconic::Policy)

target_sources(Draconic.Engine.Integration
    PRIVATE
        ScriptPhysicsContactBridge.cpp   # implementation unit: the virtual + subsystem calls
    PUBLIC
        FILE_SET integration_modules TYPE CXX_MODULES FILES
            ScriptPhysicsContactBridge.cppm)
