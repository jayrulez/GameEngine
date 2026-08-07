add_executable(Draconic.UI.Shell.Tests
    UiInputBridgeTests.cpp
    TestsMain.cpp)

target_link_libraries(Draconic.UI.Shell.Tests PRIVATE
    Draconic::UI.Shell Draconic::UI Draconic::Shell Draconic::Shell.Null doctest::doctest)

target_compile_definitions(Draconic.UI.Shell.Tests PRIVATE
    DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)

target_compile_options(Draconic.UI.Shell.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")

add_test(NAME Draconic.UI.Shell.Tests COMMAND Draconic.UI.Shell.Tests)
