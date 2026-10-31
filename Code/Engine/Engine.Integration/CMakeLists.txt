# Engine::Integration - `engine.integration`: cross-subsystem composition
# helpers owned by the app. Depends on both sides of each bridge so neither subsystem gains a
# dependency on the other, as in the bridge from physics contacts to scripts.
util_add_engine_library(Engine.Integration ALIAS Engine::Integration)
target_link_libraries(Engine.Integration
    PUBLIC Foundation::Core Foundation::Scene Foundation::Physics
           Engine::Physics Engine::Script Foundation::Policy)

target_sources(Engine.Integration
    PRIVATE
        ScriptPhysicsContactBridge.cpp   # implementation unit: the virtual and subsystem calls
    PUBLIC
        FILE_SET integration_modules TYPE CXX_MODULES FILES
            ScriptPhysicsContactBridge.cppm)
