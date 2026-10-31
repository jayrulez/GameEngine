# Foundation::Script.AngelScript - the AngelScript backend for Foundation::Script.
#
# Builds on the vendored ThirdParty/angelscript (ThirdParty::AngelScript). The SDK
# is private: consumers see only the foundation.script.angelscript module (the
# factory and its registry registration), never the AngelScript C++ API.

util_add_engine_library(Script.AngelScript ALIAS Foundation::Script.AngelScript)
target_link_libraries(Script.AngelScript
    PUBLIC Foundation::Script Foundation::Policy
    PRIVATE ThirdParty::AngelScript)

target_sources(Script.AngelScript
    PRIVATE
        AngelScriptScriptImpl.cpp   # module implementation unit: all SDK contact
    PUBLIC
        FILE_SET angelscript_modules TYPE CXX_MODULES FILES
            AngelScriptScript.cppm)

