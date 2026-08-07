# The tests REGISTER the Wren backend (an entry point's job) - the provider under test stays
# backend-neutral and resolves through the registry, exactly like the ScriptPage.
add_executable(Draconic.Editor.Script.Tests TestsMain.cpp ScriptCompletionTests.cpp ScriptApiBrowserTests.cpp)
target_link_libraries(Draconic.Editor.Script.Tests PRIVATE
    Draconic::Editor.Script Draconic::Script.Wren Draconic::Script Draconic::Script.Facades
    Draconic::UI.Toolkit Draconic::UI Draconic::Foundation
    doctest::doctest)
target_compile_definitions(Draconic.Editor.Script.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
target_compile_options(Draconic.Editor.Script.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Draconic.Editor.Script.Tests COMMAND Draconic.Editor.Script.Tests)
