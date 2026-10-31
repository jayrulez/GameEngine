# Pipeline::Script.AngelScript - `script.angelscript.pipeline` (the AngelScript
# cook service): the compile check, the shared handler scan, the [metadata] property harvest and
# the starter. Cook-only; the SDK (ThirdParty::AngelScript) is a PRIVATE dependency (the
# implementation unit includes angelscript.h and scriptbuilder for the metadata harvest, which
# keeps them out of GCC's module interface). The editor colours AngelScript from its
# definition in Data/Editor/Code.
util_add_engine_library(Script.AngelScript.Pipeline ALIAS Pipeline::Script.AngelScript)
target_link_libraries(Script.AngelScript.Pipeline
    PUBLIC
        Foundation::Core Pipeline::Core Foundation::Content
        Foundation::Script Foundation::Script.Resource Foundation::Script.Facades
        Pipeline::Script Foundation::Script.AngelScript Foundation::Policy
    PRIVATE
        ThirdParty::AngelScript)
target_sources(Script.AngelScript.Pipeline
    PRIVATE
        AngelScriptScriptCookImpl.cpp
    PUBLIC
        FILE_SET script_as_pipeline_modules TYPE CXX_MODULES FILES AngelScriptScriptCook.cppm)
