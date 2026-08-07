# Draconic::Fonts.DistanceField unit tests (doctest).

add_executable(Draconic.Fonts.DistanceField.Tests TestsMain.cpp DFFontAtlasTests.cpp)
target_link_libraries(Draconic.Fonts.DistanceField.Tests PRIVATE Draconic::Fonts.DistanceField Draconic::Fonts doctest::doctest)
target_compile_definitions(Draconic.Fonts.DistanceField.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
target_compile_options(Draconic.Fonts.DistanceField.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>" "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Draconic.Fonts.DistanceField.Tests COMMAND Draconic.Fonts.DistanceField.Tests)
