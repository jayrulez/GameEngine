# Foundation::Script - the `foundation.script` module.
#
# A thin, VM-agnostic scripting abstraction over Core's reflection. Concrete VM
# backends (AngelScript, Luau) are separate libraries implementing these interfaces.

util_add_engine_library(Script ALIAS Foundation::Script)
# PUBLIC Foundation::Core: propagates the reflection ABI and the Code include
# root (so module GMFs can #include "Core/Prelude.h").
target_link_libraries(Script PUBLIC Foundation::Core Foundation::Policy)

# Module implementation unit: the current-script-context slot (one per process).
target_sources(Script PRIVATE ScriptContextImpl.cpp)

target_sources(Script
    PUBLIC
        FILE_SET script_modules TYPE CXX_MODULES FILES
            ScriptModule.cppm
            IScriptDelegate.cppm
            ScriptIntrospection.cppm
            ScriptDebug.cppm
            IScriptContext.cppm
            IScriptManager.cppm
            RegisterTypes.cppm
            BackendRegistry.cppm)
