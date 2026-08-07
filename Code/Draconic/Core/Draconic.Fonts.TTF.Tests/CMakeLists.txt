# Draconic::Fonts.TTF unit tests (doctest). Uses the bundled Roboto font asset.

add_executable(Draconic.Fonts.TTF.Tests TestsMain.cpp TrueTypeTests.cpp UIFeaturesTests.cpp)
target_link_libraries(Draconic.Fonts.TTF.Tests PRIVATE Draconic::Fonts.TTF Draconic::Fonts.IO Draconic::Fonts doctest::doctest)
target_compile_definitions(Draconic.Fonts.TTF.Tests PRIVATE
    DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
    DRACONIC_FONTS_ASSET_DIR="${CMAKE_SOURCE_DIR}/Data/Assets/fonts")
target_compile_options(Draconic.Fonts.TTF.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>" "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Draconic.Fonts.TTF.Tests COMMAND Draconic.Fonts.TTF.Tests)
