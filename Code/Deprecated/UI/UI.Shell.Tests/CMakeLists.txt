add_executable(UI.Shell.Tests
    UiInputBridgeTests.cpp
    TestsMain.cpp)

target_link_libraries(UI.Shell.Tests PRIVATE
    Foundation::UI.Shell Foundation::UI Foundation::Shell Foundation::Shell.Null doctest::doctest)

target_compile_definitions(UI.Shell.Tests PRIVATE
    DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)

target_compile_options(UI.Shell.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")

add_test(NAME UI.Shell.Tests COMMAND UI.Shell.Tests)
