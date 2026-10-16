# Foundation::Fonts.HarfBuzz unit tests (doctest): shaping over the bundled Roboto (an fi ligature)
# and DejaVu Sans Mono (Arabic joining), through the engine's ITextShaper.

add_executable(Fonts.HarfBuzz.Tests TestsMain.cpp HarfBuzzShaperTests.cpp)
target_link_libraries(Fonts.HarfBuzz.Tests PRIVATE Foundation::VFS Foundation::Fonts.HarfBuzz Foundation::Fonts.TrueType Foundation::Fonts.DistanceField.Baker Foundation::Fonts doctest::doctest)
target_compile_definitions(Fonts.HarfBuzz.Tests PRIVATE
    DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
target_compile_options(Fonts.HarfBuzz.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>" "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Fonts.HarfBuzz.Tests COMMAND Fonts.HarfBuzz.Tests)
