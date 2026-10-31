# Foundation::Script unit tests (doctest).

add_executable(Script.Tests
    TestsMain.cpp
    ScriptTests.cpp)

target_link_libraries(Script.Tests PRIVATE Foundation::Script doctest::doctest)

target_compile_definitions(Script.Tests PRIVATE
    DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)

# doctest 2.4.11 <ciso646> deprecation #warning under C++23: third-party noise.
target_compile_options(Script.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")

add_test(NAME Script.Tests COMMAND Script.Tests)
