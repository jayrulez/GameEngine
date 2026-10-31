# Foundation::Script.AngelScript unit tests (doctest): the AngelScript backend's own suite, so it
# builds only when AngelScript is on (a raw Foundation::Script.AngelScript link would break configure
# in a Luau-only build). Links the Luau backend too (guarded alias): the registry test proves both
# backends resolve side by side.
if(OPTION_ENABLE_ANGELSCRIPT)
    add_executable(Script.AngelScript.Tests
        TestsMain.cpp
        ScriptAngelScriptTests.cpp)

    target_link_libraries(Script.AngelScript.Tests PRIVATE
        ${SCRIPTLIB_ANGELSCRIPT} ${SCRIPTLIB_LUAU} doctest::doctest)

    target_compile_definitions(Script.AngelScript.Tests PRIVATE
        DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)

    # doctest 2.4.11 <ciso646> deprecation #warning under C++23: third-party noise.
    target_compile_options(Script.AngelScript.Tests PRIVATE
        "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
        "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")

    add_test(NAME Script.AngelScript.Tests COMMAND Script.AngelScript.Tests)
endif()
