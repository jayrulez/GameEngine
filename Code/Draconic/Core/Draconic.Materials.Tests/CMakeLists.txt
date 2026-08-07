# Draconic::Materials unit tests (doctest; Null RHI - no GPU/DXC).

add_executable(Draconic.Materials.Tests
    TestsMain.cpp
    MaterialsTests.cpp)

target_link_libraries(Draconic.Materials.Tests PRIVATE
    Draconic::Materials Draconic::RHI.Null doctest::doctest)

target_compile_definitions(Draconic.Materials.Tests PRIVATE
    DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)

target_compile_options(Draconic.Materials.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")

add_test(NAME Draconic.Materials.Tests COMMAND Draconic.Materials.Tests)
