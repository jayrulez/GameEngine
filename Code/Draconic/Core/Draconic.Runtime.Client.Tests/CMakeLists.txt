# Draconic::Runtime.Client unit tests (doctest).

add_executable(Draconic.Runtime.Client.Tests
    TestsMain.cpp
    ApplicationTests.cpp)

target_link_libraries(Draconic.Runtime.Client.Tests PRIVATE
    Draconic::Runtime.Client Draconic::Shell.Null Draconic::Graphics Draconic::Graphics.Null doctest::doctest)

target_compile_definitions(Draconic.Runtime.Client.Tests PRIVATE
    DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)

target_compile_options(Draconic.Runtime.Client.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")

add_test(NAME Draconic.Runtime.Client.Tests COMMAND Draconic.Runtime.Client.Tests)
