# The tests register the AngelScript cook, as an entry point does; the library under test
# stays backend-neutral and resolves through the registry. The AngelScript cook's own
# suite, so it builds only when AngelScript is on.
if(OPTION_ENABLE_ANGELSCRIPT)
    add_executable(Script.AngelScript.Pipeline.Tests TestsMain.cpp AngelScriptCookTests.cpp)
    target_link_libraries(Script.AngelScript.Pipeline.Tests PRIVATE
        Pipeline::Script.AngelScript
        Foundation::Script.AngelScript Pipeline::Script
        Foundation::Script.Resource Foundation::Script Foundation::Script.Facades
        Engine::Composition # RegisterAllScriptFacades: the complete engine facade surface the
                              # run:: and ui:: regression cook compiles against
        doctest::doctest)
    target_compile_definitions(Script.AngelScript.Pipeline.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
    target_compile_options(Script.AngelScript.Pipeline.Tests PRIVATE
        "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
        "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
    add_test(NAME Script.AngelScript.Pipeline.Tests COMMAND Script.AngelScript.Pipeline.Tests)
endif()
