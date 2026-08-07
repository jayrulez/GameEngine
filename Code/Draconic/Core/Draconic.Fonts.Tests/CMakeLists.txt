# Draconic::Fonts unit tests (doctest).

add_executable(Draconic.Fonts.Tests TestsMain.cpp FontsTypesTests.cpp)
target_link_libraries(Draconic.Fonts.Tests PRIVATE Draconic::Fonts doctest::doctest)
target_compile_definitions(Draconic.Fonts.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
target_compile_options(Draconic.Fonts.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>" "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Draconic.Fonts.Tests COMMAND Draconic.Fonts.Tests)
