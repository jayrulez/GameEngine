# Pipeline::Script.AngelScript - `script.angelscript.pipeline` (the AngelScript
# cook service): compile-check + shared handler scan + [metadata] PROPERTY HARVEST + starter.
# Cook-only; the SDK (ThirdParty::AngelScript) is a PRIVATE dep (impl unit #includes angelscript.h
# + scriptbuilder for metadata harvest - GCC module hygiene). The editor colours AngelScript from its
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
