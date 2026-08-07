# Draconic::Geometry unit tests (doctest; pure data, no GPU).

add_executable(Draconic.Geometry.Tests
    TestsMain.cpp
    GeometryTests.cpp)

target_link_libraries(Draconic.Geometry.Tests PRIVATE
    Draconic::Geometry doctest::doctest)

target_compile_definitions(Draconic.Geometry.Tests PRIVATE
    DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)

target_compile_options(Draconic.Geometry.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")

add_test(NAME Draconic.Geometry.Tests COMMAND Draconic.Geometry.Tests)
