add_executable(Draconic.Physics.Tests
    TestsMain.cpp
    PhysicsTests.cpp)

target_link_libraries(Draconic.Physics.Tests PRIVATE
    Draconic::Physics doctest::doctest)

target_compile_definitions(Draconic.Physics.Tests PRIVATE
    DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)

target_compile_options(Draconic.Physics.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")

add_test(NAME Draconic.Physics.Tests COMMAND Draconic.Physics.Tests)
