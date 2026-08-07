# Draconic::Script.Wren unit tests (doctest).

add_executable(Draconic.Script.Wren.Tests
    TestsMain.cpp
    ScriptWrenTests.cpp)

target_link_libraries(Draconic.Script.Wren.Tests PRIVATE Draconic::Script.Wren doctest::doctest)

target_compile_definitions(Draconic.Script.Wren.Tests PRIVATE
    DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)

# doctest 2.4.11 <ciso646> deprecation #warning under C++23 - third-party noise.
target_compile_options(Draconic.Script.Wren.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")

add_test(NAME Draconic.Script.Wren.Tests COMMAND Draconic.Script.Wren.Tests)
