# Foundation::Heightfield unit tests (doctest; pure data and math, no GPU).

add_executable(Heightfield.Tests
    TestsMain.cpp
    HeightfieldTests.cpp)

target_link_libraries(Heightfield.Tests PRIVATE
    Foundation::Heightfield doctest::doctest)

target_compile_definitions(Heightfield.Tests PRIVATE
    DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)

target_compile_options(Heightfield.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")

add_test(NAME Heightfield.Tests COMMAND Heightfield.Tests)
