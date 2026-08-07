# Draconic::Shell.Null unit tests (doctest).

add_executable(Draconic.Shell.Null.Tests
    TestsMain.cpp
    NullShellTests.cpp)

target_link_libraries(Draconic.Shell.Null.Tests PRIVATE
    Draconic::Shell.Null doctest::doctest)

target_compile_definitions(Draconic.Shell.Null.Tests PRIVATE
    DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)

target_compile_options(Draconic.Shell.Null.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")

add_test(NAME Draconic.Shell.Null.Tests COMMAND Draconic.Shell.Null.Tests)
