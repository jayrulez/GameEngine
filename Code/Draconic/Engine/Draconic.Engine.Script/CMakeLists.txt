# Draconic::Engine.Script - `draconic.engine.script`: entity behaviors
# (ScriptComponent + per-scene dispatch + the run's ONE gameplay script context).
# Backend-neutral: links Draconic::Script for the REGISTRY only - entry points
# register concrete backends.
add_library(Draconic.Engine.Script STATIC)
add_library(Draconic::Engine.Script ALIAS Draconic.Engine.Script)

target_link_libraries(Draconic.Engine.Script
    PUBLIC Draconic::Foundation Draconic::Runtime Draconic::Scene Draconic::Engine.Scene
           Draconic::Script Draconic::Script.Resource Draconic::Script.Facades
           Draconic::Resource Draconic::Content
           Draconic::Profiler Draconic::Policy)

target_sources(Draconic.Engine.Script
    PRIVATE
        ScriptSubsystemImpl.cpp   # implementation unit: the DRACONIC_REFLECT_* bodies (GCC gcm rule)
    PUBLIC
        FILE_SET script_subsystem_modules TYPE CXX_MODULES FILES
            ScriptSubsystem.cppm
            ScriptComponents.cppm)

