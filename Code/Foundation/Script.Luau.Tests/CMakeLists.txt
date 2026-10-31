# Foundation::Script.Luau unit tests (doctest): the shared conformance battery with the Luau
# dialect. The Luau backend's own suite, so it builds only when Luau is on.
if(OPTION_ENABLE_LUAU)
    add_executable(Script.Luau.Tests
        TestsMain.cpp
        ScriptLuauTests.cpp)

    target_link_libraries(Script.Luau.Tests PRIVATE Foundation::Script.Luau doctest::doctest)

    target_compile_definitions(Script.Luau.Tests PRIVATE
        DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)

    # doctest 2.4.11 <ciso646> deprecation #warning under C++23: third-party noise.
    target_compile_options(Script.Luau.Tests PRIVATE
        "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
        "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")

    add_test(NAME Script.Luau.Tests COMMAND Script.Luau.Tests)
endif()
