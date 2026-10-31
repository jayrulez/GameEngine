# Foundation::Materials unit tests (doctest; Null RHI, no GPU or DXC).

add_executable(Materials.Tests
    TestsMain.cpp
    MaterialsTests.cpp)

target_link_libraries(Materials.Tests PRIVATE
    Foundation::Materials Foundation::RHI.Null doctest::doctest)

target_compile_definitions(Materials.Tests PRIVATE
    DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)

target_compile_options(Materials.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")

add_test(NAME Materials.Tests COMMAND Materials.Tests)
