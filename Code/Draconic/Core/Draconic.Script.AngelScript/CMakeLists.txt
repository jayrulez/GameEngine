# Draconic::Script.AngelScript - the AngelScript backend for Draconic::Script.
#
# Builds on the vendored ThirdParty/angelscript (Draconic::AngelScript). The SDK
# is PRIVATE: consumers see only the draconic.script.angelscript module (the
# factory + registry registration), never the AngelScript C++ API.

add_library(Draconic.Script.AngelScript STATIC)
add_library(Draconic::Script.AngelScript ALIAS Draconic.Script.AngelScript)

target_link_libraries(Draconic.Script.AngelScript
    PUBLIC Draconic::Script Draconic::Policy
    PRIVATE Draconic::AngelScript)

target_sources(Draconic.Script.AngelScript
    PRIVATE
        AngelScriptScriptImpl.cpp   # module IMPLEMENTATION unit: all SDK contact
    PUBLIC
        FILE_SET angelscript_modules TYPE CXX_MODULES FILES
            AngelScriptScript.cppm)

