# Editor::Script tests: the API browser's tree, the language service (completion and hover) on each
# backend, and the script page over its markup. They REGISTER a backend (an entry point's job); the
# page and the service stay backend-neutral and resolve through the registries, as the editor does.
add_executable(Editor.Script.Tests TestsMain.cpp ScriptApiBrowserTests.cpp ScriptLanguageServiceTests.cpp ScriptPageTests.cpp)
target_link_libraries(Editor.Script.Tests PRIVATE
    Editor::Script Editor::App Editor::Core
    ${SCRIPTLIB_ANGELSCRIPT} ${SCRIPTLIB_LUAU} ${SCRIPTLIB_ANGELSCRIPT_PIPELINE} Foundation::Script Foundation::Script.Facades
    Pipeline::Script Foundation::Content Foundation::VFS Foundation::GUI Foundation::GUI.Toolkit Foundation::GUI.Code Foundation::Core
    doctest::doctest)
target_compile_definitions(Editor.Script.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
    RAPTOR_SOURCE_DIR="${CMAKE_SOURCE_DIR}") # the page's markup and the languages the tests load
target_compile_options(Editor.Script.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Editor.Script.Tests COMMAND Editor.Script.Tests)
