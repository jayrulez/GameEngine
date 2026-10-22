add_executable(GUI.Code.Tests
    CodeDocumentTests.cpp
    TestsMain.cpp)

target_link_libraries(GUI.Code.Tests PRIVATE Experimental::GUI.Code Experimental::GUI doctest::doctest)

target_compile_definitions(GUI.Code.Tests PRIVATE
    DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)

target_compile_options(GUI.Code.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")

add_test(NAME GUI.Code.Tests COMMAND GUI.Code.Tests)
