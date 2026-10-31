# Foundation::Run unit tests (doctest; pure logic: runs, sessions, services, the registry).

add_executable(Run.Tests
    TestsMain.cpp
    RunTests.cpp)

target_link_libraries(Run.Tests PRIVATE
    Foundation::Run Foundation::Messaging doctest::doctest)

target_compile_definitions(Run.Tests PRIVATE
    DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)

target_compile_options(Run.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")

add_test(NAME Run.Tests COMMAND Run.Tests)
