add_executable(GUI.Toolkit.Tests
    ToolBarTests.cpp
    StatusBarTests.cpp
    PropertySourceTests.cpp
    PropertyEditorTests.cpp
    TestsMain.cpp)

target_link_libraries(GUI.Toolkit.Tests PRIVATE Experimental::GUI.Toolkit Experimental::GUI Foundation::Xml doctest::doctest)

target_compile_definitions(GUI.Toolkit.Tests PRIVATE
    DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)

target_compile_options(GUI.Toolkit.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")

add_test(NAME GUI.Toolkit.Tests COMMAND GUI.Toolkit.Tests)
