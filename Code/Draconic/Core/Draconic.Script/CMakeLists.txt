# Draconic::Script - the `draconic.script` module.
#
# A thin, VM-agnostic scripting abstraction over Foundation's reflection. Concrete VM
# backends (Lua, ...) are separate plugins implementing these interfaces.

add_library(Draconic.Script STATIC)
add_library(Draconic::Script ALIAS Draconic.Script)

# PUBLIC Draconic::Foundation: propagates the reflection ABI + the Code/Draconic include
# root (so module GMFs can #include "Foundation/Prelude.h").
target_link_libraries(Draconic.Script PUBLIC Draconic::Foundation Draconic::Policy)

target_sources(Draconic.Script
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
