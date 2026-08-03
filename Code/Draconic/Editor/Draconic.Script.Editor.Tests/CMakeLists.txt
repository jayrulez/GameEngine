# The tests REGISTER the Wren backend (an entry point's job) - the library under test
# stays backend-neutral and resolves through the registry (B3).
add_executable(DraconicScriptPipelineTests TestsMain.cpp ScriptPipelineTests.cpp ScriptReflectionTests.cpp)
target_link_libraries(DraconicScriptPipelineTests PRIVATE
    Draconic::Script.Editor Draconic::Script.Wren.Editor Draconic::Script.Resource Draconic::Script
    Draconic::Script.Wren Draconic::Script.Facades
    Draconic::Editor Draconic::Editor.Core Draconic::Content Draconic::Resource Draconic::VFS
    doctest::doctest)
target_compile_definitions(DraconicScriptPipelineTests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
target_compile_options(DraconicScriptPipelineTests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME DraconicScriptPipelineTests COMMAND DraconicScriptPipelineTests)
