# Engine::Script - `engine.script`: entity behaviors
# (ScriptComponent, per-scene dispatch, the run's single gameplay script context).
# Backend-neutral: links Foundation::Script only for the registry; entry points
# register concrete backends.
util_add_engine_library(Engine.Script ALIAS Engine::Script)
target_link_libraries(Engine.Script
    PUBLIC Foundation::Core Foundation::Runtime Foundation::Scene Engine::Scene
           Foundation::Script Foundation::Script.Resource Foundation::Script.Facades
           Foundation::Resource Foundation::Content
           Foundation::Profiler Engine::Domain Foundation::Policy)

target_sources(Engine.Script
    PRIVATE
        ScriptSubsystemImpl.cpp   # implementation unit: the REFLECT_* bodies (GCC gcm rule)
    PUBLIC
        FILE_SET script_subsystem_modules TYPE CXX_MODULES FILES
            ScriptSubsystem.cppm
            ScriptComponents.cppm)

