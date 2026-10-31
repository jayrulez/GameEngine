# Foundation::Script.Facades - `foundation.script.facades` (the curated behavior facades:
# Entity/Log/Time/Random and the run-service binding). Shared by the runtime subsystem
# and the editor's cook so both VMs register the same "main"-module surface.
util_add_engine_library(Script.Facades ALIAS Foundation::Script.Facades)
target_link_libraries(Script.Facades PUBLIC
    Foundation::Core Foundation::Scene Foundation::Scene.Resource Foundation::Script Foundation::Policy)
target_sources(Script.Facades
    PRIVATE
        ScriptFacadesImpl.cpp   # implementation unit: the REFLECT_* bodies (GCC gcm rule)
    PUBLIC
        FILE_SET script_facades_modules TYPE CXX_MODULES FILES ScriptFacades.cppm)
