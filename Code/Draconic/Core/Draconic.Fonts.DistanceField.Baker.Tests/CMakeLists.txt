# Draconic::Fonts.DistanceField.Baker tests (doctest) - bakes real glyphs and inspects MSDF cell placement.

add_executable(Draconic.Fonts.DistanceField.Baker.Tests TestsMain.cpp DFBakerTests.cpp)
target_link_libraries(Draconic.Fonts.DistanceField.Baker.Tests PRIVATE
    Draconic::Fonts.DistanceField.Baker Draconic::Fonts.DistanceField Draconic::Fonts Draconic::Fonts.IO Draconic::Fonts.TTF
    doctest::doctest)
target_compile_definitions(Draconic.Fonts.DistanceField.Baker.Tests PRIVATE
    DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
    DRACONIC_FONTS_ASSET_DIR="${CMAKE_SOURCE_DIR}/Data/Assets/fonts")
target_compile_options(Draconic.Fonts.DistanceField.Baker.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>" "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Draconic.Fonts.DistanceField.Baker.Tests COMMAND Draconic.Fonts.DistanceField.Baker.Tests)
